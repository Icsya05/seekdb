#!/usr/bin/env python3
"""Compile the actual IK header with a small platform shim, without a server build.

The real charset helper, mb_wc UTF-8 decoder and unicode_utils.cpp are included.
Only logging, charset dispatch and unused string-conversion declarations are
shimmed. This is a focused header test, not database or full-build validation.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


BASELINE = "5bce9e06d1281347ad61344e4b01fab9158c58a2"
ROOT = Path(__file__).resolve().parents[3]
HEADER = "src/storage/fts/ik/ob_ik_char_util.h"


def write(root, path, text):
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")


def prepare_shim(root):
    charset = (ROOT / "src/oblib/lib/charset/ob_charset.h").read_text(encoding="utf-8")
    enums = "\n".join(
        re.search(r"enum " + name + r"\s*\{.*?\n\};", charset, re.S).group(0)
        for name in ("ObCharsetType", "ObCollationType")
    )
    write(root, "lib/charset/ob_ctype.h", r"""#pragma once
#include <cstdint>
#include <cstring>
using ob_wc_t = unsigned long;
using uint16 = uint16_t;
using uint32 = uint32_t;
#define OB_CS_ILSEQ 0
#define OB_CS_ILUNI 0
#define OB_CS_TOOSMALL -101
#define OB_CS_TOOSMALL2 -102
#define OB_CS_TOOSMALL3 -103
#define OB_CS_TOOSMALL4 -104
#define OB_CS_TOOSMALLN(n) (-100-(n))
struct ObCharsetInfo;
struct TestCharsetHandler {
  int (*mb_wc)(const ObCharsetInfo *, ob_wc_t *, const unsigned char *, const unsigned char *);
};
struct ObCharsetInfo { TestCharsetHandler *cset; };
""")
    write(root, "lib/utility/ob_macro_utils.h", r"""#pragma once
#include "lib/ob_errno.h"
#define OB_LIKELY(x) (x)
#define OB_UNLIKELY(x) (x)
#define OB_ISNULL(x) (nullptr == (x))
#define OB_NOT_NULL(x) (nullptr != (x))
#define OB_FAIL(x) (oceanbase::common::OB_SUCCESS != (ret = (x)))
#define OB_SUCC(x) (oceanbase::common::OB_SUCCESS == (x))
#define UNUSED(x) (void)(x)
#define ARRAYSIZEOF(x) (sizeof(x) / sizeof((x)[0]))
""")
    write(root, "lib/oblog/ob_log_module.h", "#pragma once\n#define STORAGE_FTS_LOG(...)\n")
    write(root, "lib/charset/ob_charset.h", r"""#pragma once
#include "lib/charset/ob_ctype.h"
#include "lib/utility/ob_macro_utils.h"
namespace oceanbase { namespace common {
""" + enums + r"""
template<class T, class U> T pointer_cast(U p) { return reinterpret_cast<T>(p); }
class ObString {
public:
  ObString(int64_t n, const char *p) : n_(n), p_(p) {}
  const char *ptr() const { return p_; }
  int64_t length() const { return n_; }
private:
  int64_t n_;
  const char *p_;
};
class ObCharset {
public:
  static constexpr int MAX_MB_LEN = 5;
  static ObCharsetType charset_type_by_coll(ObCollationType coll) {
    switch (coll) {
    case CS_TYPE_UTF8MB4_GENERAL_CI: case CS_TYPE_UTF8MB4_BIN: return CHARSET_UTF8MB4;
    case CS_TYPE_UTF16_GENERAL_CI: case CS_TYPE_UTF16_BIN: return CHARSET_UTF16;
    case CS_TYPE_UTF16LE_GENERAL_CI: case CS_TYPE_UTF16LE_BIN: return CHARSET_UTF16LE;
    case CS_TYPE_GBK_BIN: return CHARSET_GBK;
    case CS_TYPE_GB18030_BIN: return CHARSET_GB18030;
    case CS_TYPE_LATIN1_BIN: return CHARSET_LATIN1;
    default: return CHARSET_INVALID;
    }
  }
  static int charset_convert(ObCollationType, const char *, int64_t,
      ObCollationType, char *, int64_t, uint32_t &, bool, bool, ob_wc_t);
  static int wc_mb(ObCollationType, int32_t, char *, int32_t, int32_t &);
};
}}
using namespace oceanbase::common;
""")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("g++") or shutil.which("clang++"))
    parser.add_argument("--git", default="git", help="Git executable; use git.exe for a Windows worktree under WSL")
    parser.add_argument("--optimization", choices=("0", "2"), default="2")
    parser.add_argument("--equivalence-only", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("provide --cxx or install a C++17 compiler")
    with tempfile.TemporaryDirectory(prefix="seekdb-ik-test-") as temp:
        temp = Path(temp)
        prepare_shim(temp)
        original = subprocess.check_output(
            [args.git, "show", f"{BASELINE}:{HEADER}"], cwd=ROOT
        ).decode("utf-8")
        write(temp, "legacy_ik_char_util.h", original.replace("ObFTCharUtil", "LegacyFTCharUtil")
              .replace("_OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_", "TEST_LEGACY_IK_CHAR_UTIL_H"))
        exe = temp / "ik_char_util_test.exe"
        command = [args.cxx, "-std=c++17", f"-O{args.optimization}", "-Wall", "-Wextra",
                   "-I" + str(temp), "-I" + str(ROOT), "-I" + str(ROOT / "src"),
                   "-I" + str(ROOT / "src/oblib"),
                   str(Path(__file__).with_name("test_ik_char_util.cpp")), "-o", str(exe)]
        print("Compile:", subprocess.list2cmdline(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(exe)] + (["--equivalence-only"] if args.equivalence_only else []), check=True)


if __name__ == "__main__":
    main()
