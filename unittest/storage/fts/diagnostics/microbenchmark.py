"""Diagnostic only: actual IK header, balanced uninstrumented AB/BA microbenchmark.

Requires Linux GCC, Python 3 and the focused test runner in --repo. This is not
the competition benchmark and does not produce or predict a competition score.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--repo", type=Path, required=True)
parser.add_argument("--git", default="git")
parser.add_argument("--cxx", default="/usr/bin/g++")
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
repo = args.repo.resolve()
spec = importlib.util.spec_from_file_location("ik_test", repo / "unittest/storage/fts/run_ik_char_util_test.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
git = args.git
prefix = (repo / "unittest/storage/fts/test_ik_char_util.cpp").read_text().split("using namespace oceanbase::common;")[0]
prefix = prefix.replace("  ++decode_calls;", "")
body = r'''
#include <chrono>
#include <vector>
#include <algorithm>
#include <iomanip>
using namespace oceanbase::common;
using namespace oceanbase::storage;
struct Character { const char *data; uint8_t length; };
struct Run { double ms; uint64_t checksum; };
template<bool Legacy>
Run run(const std::vector<Character> &characters, uint64_t rounds)
{
  uint64_t checksum = 0x123456789;
  auto start = std::chrono::steady_clock::now();
  for (uint64_t round = 0; round < rounds; ++round) {
    size_t index = (round * 31) % characters.size();
    for (size_t pos = 0; pos < characters.size(); ++pos) {
      const auto &c = characters[index];
      int ret;
      int type;
      if constexpr (Legacy) {
        auto classification = LegacyFTCharUtil::CharType::USELESS;
        ret = LegacyFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, c.data, c.length, classification);
        type = static_cast<int>(classification);
      } else {
        auto classification = ObFTCharUtil::CharType::USELESS;
        ret = ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, c.data, c.length, classification);
        type = static_cast<int>(classification);
      }
      checksum = (checksum * 1099511628211ULL) ^ uint64_t(type + ret);
      if (++index == characters.size()) { index = 0; }
    }
  }
  return {std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), checksum};
}
int main()
{
  // Runtime stream input prevents a compiler from specializing on benchmark text.
  std::string label;
  std::string text;
  while (std::getline(std::cin, label) && std::getline(std::cin, text)) {
    std::vector<Character> chars;
    auto *p = reinterpret_cast<const unsigned char *>(text.data());
    auto *end = p + text.size();
    while (p < end) {
      int len = ob_charset_char_len<CHARSET_UTF8MB4>(p, end);
      if (len <= 0) { return 2; }
      chars.push_back({reinterpret_cast<const char *>(p), uint8_t(len)});
      p += len;
    }
    uint64_t rounds = 20000000 / chars.size();
    auto a = run<true>(chars, 1000);
    auto b = run<false>(chars, 1000);
    if (a.checksum != b.checksum) { return 3; }
    for (int pair = 0; pair < 6; ++pair) {
      if (pair % 2 == 0) { a = run<true>(chars, rounds); b = run<false>(chars, rounds); }
      else { b = run<false>(chars, rounds); a = run<true>(chars, rounds); }
      if (a.checksum != b.checksum) { return 4; }
      std::cout << label << ',' << pair << ',' << a.ms << ',' << b.ms << ',' << a.checksum << '\n';
    }
  }
}
'''
texts = {
    "ascii": "The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path. Database 12345 performance audit. ",
    "chinese": "全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低处理开销。",
    "mixed": "OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。",
    "punctuation": "\t !?#&+-.,@_()[]{}<>/=|:;\"'　，。！？；：（）【】「」…— 😀🚀 ",
}
with tempfile.TemporaryDirectory(prefix="seekdb-fts-micro-") as tmp:
    tmp = Path(tmp)
    module.prepare_shim(tmp)
    original = subprocess.check_output([git, "show", f"{module.BASELINE}:{module.HEADER}"], cwd=repo).decode()
    module.write(tmp, "legacy_ik_char_util.h", original.replace("ObFTCharUtil", "LegacyFTCharUtil").replace("_OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_", "TEST_LEGACY_IK_CHAR_UTIL_H"))
    module.write(tmp, "micro.cpp", prefix + body)
    output = {"metadata": {
        "compiler": subprocess.check_output([args.cxx, "--version"], text=True).splitlines()[0],
        "platform": platform.platform(),
        "baseline": module.BASELINE,
        "production_header_sha256": hashlib.sha256((repo / module.HEADER).read_bytes()).hexdigest(),
        "text_corpora": texts,
        "characters_per_sample_approximately": 20000000,
        "pairs_per_corpus": 6,
        "order": "AB,BA,AB,BA,AB,BA",
        "instrumentation": "decoder counter disabled; no noinline, volatile or assembly barriers",
        "limitations": "platform/logging/dispatch shim from focused unit test; single-process header diagnostic, not database benchmark"
    }}
    for optimization in ("2", "3"):
        command = [args.cxx, "-std=c++17", "-O" + optimization,
                   "-I" + str(tmp), "-I" + str(repo), "-I" + str(repo / "src"),
                   "-I" + str(repo / "src/oblib"), str(tmp / "micro.cpp"), "-o", str(tmp / "micro")]
        subprocess.run(command, check=True)
        result = subprocess.run([str(tmp / "micro")], input="".join(k + "\n" + v + "\n" for k, v in texts.items()),
                                text=True, capture_output=True, check=True)
        print("O" + optimization + "\n" + result.stdout, flush=True)
        output["O" + optimization] = result.stdout
    args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
