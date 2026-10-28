/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */

// Run through run_ik_char_util_test.py. Production classification, Unicode
// predicates and decoder bodies are compiled directly, without copying them.
#include <cstdlib>
#include <iostream>
#include <string>

#define ob_charset_decode_unicode uncounted_charset_decode_unicode
#include "lib/charset/ob_charset_string_helper.h"
#undef ob_charset_decode_unicode

namespace oceanbase { namespace common {
static uint64_t decode_calls = 0;
template<ObCharsetType CS_TYPE>
int ob_charset_decode_unicode(const unsigned char *s, const unsigned char *e, ob_wc_t &unicode)
{
  ++decode_calls;
  return uncounted_charset_decode_unicode<CS_TYPE>(s, e, unicode);
}
}}

#include "legacy_ik_char_util.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/utils/unicode_utils.cpp"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using CharType = ObFTCharUtil::CharType;
static uint64_t comparisons = 0;

static void require(bool ok, const char *message)
{
  if (!ok) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

static void compare(ObCollationType coll, const char *input, uint8_t length)
{
  CharType actual = CharType::SURROGATE_LOW;
  auto expected = LegacyFTCharUtil::CharType::SURROGATE_LOW;
  const int old_ret = LegacyFTCharUtil::classify_first_char(coll, input, length, expected);
  const int ret = ObFTCharUtil::classify_first_char(coll, input, length, actual);
  if (old_ret != ret || static_cast<int>(expected) != static_cast<int>(actual)) {
    std::cerr << "Mismatch: collation=" << coll << " length=" << int(length)
              << " old_ret=" << old_ret << " ret=" << ret << '\n';
    require(false, "classification must match frozen competition implementation");
  }
  ++comparisons;
}

static void check_literal_categories()
{
  struct Fixture { const char *text; CharType type; };
  const Fixture fixtures[] = {
    {"A", CharType::ENGLISH_LETTER}, {"z", CharType::ENGLISH_LETTER},
    {"9", CharType::ARABIC_LETTER}, {"\xef\xbc\xa1", CharType::ENGLISH_LETTER},
    {"\xef\xbd\x9a", CharType::ENGLISH_LETTER}, {"\xef\xbc\x99", CharType::ARABIC_LETTER},
    {"\xe4\xb8\xad", CharType::CHINESE}, {"\xe3\x81\x82", CharType::OTHER_CJK},
    {"\xea\xb0\x80", CharType::OTHER_CJK}, {" ", CharType::USELESS},
    {"\xf0\x9f\x98\x80", CharType::USELESS}
  };
  for (const auto &f : fixtures) {
    CharType type = CharType::USELESS;
    require(OB_SUCCESS == ObFTCharUtil::classify_first_char(
        CS_TYPE_UTF8MB4_BIN, f.text, std::strlen(f.text), type), "fixture return code");
    require(type == f.type, "literal category including fullwidth precedence");
  }
}

static void check_equivalence()
{
  unsigned char bytes[4] = {};
  // Every Unicode scalar, encoded using the real product encoder.
  for (uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) { continue; }
    const int n = ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp, bytes, bytes + 4);
    require(n > 0, "valid scalar encoding");
    compare(CS_TYPE_UTF8MB4_BIN, reinterpret_cast<char *>(bytes), n);
  }
  // All single-byte and two-byte sequences, valid, truncated or malformed.
  const ObCollationType colls[] = {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN};
  for (auto coll : colls) {
    compare(coll, nullptr, 0);
    compare(coll, nullptr, 4);
    compare(coll, reinterpret_cast<char *>(bytes), 0);
    for (unsigned a = 0; a < 256; ++a) {
      bytes[0] = a;
      compare(coll, reinterpret_cast<char *>(bytes), 1);
      for (unsigned b = 0; b < 256; ++b) {
        bytes[1] = b;
        compare(coll, reinterpret_cast<char *>(bytes), 2);
      }
    }
  }
  // Longer malformed sequences: overlong, surrogate, out-of-range and truncation.
  const unsigned char malformed[][4] = {
    {0xe0, 0x80, 0x80, 0}, {0xed, 0xa0, 0x80, 0}, {0xf0, 0x80, 0x80, 0x80},
    {0xf4, 0x90, 0x80, 0x80}, {0xff, 0xff, 0xff, 0xff}, {0xe4, 0, 0xad, 0},
    {0xf0, 0x9f, 0x98, 0x80}, {0xe4, 0xb8, 0xad, 0}, {0, 0, 0, 0}
  };
  for (auto coll : colls) {
    for (const auto &item : malformed) {
      for (uint8_t n = 0; n <= 4; ++n) {
        compare(coll, reinterpret_cast<const char *>(item), n);
      }
    }
  }
  const ObCollationType unsupported[] = {
    CS_TYPE_GBK_BIN, CS_TYPE_GB18030_BIN, CS_TYPE_LATIN1_BIN, CS_TYPE_BINARY, CS_TYPE_INVALID
  };
  for (auto coll : unsupported) {
    CharType type = CharType::CHINESE;
    require(OB_NOT_SUPPORTED == ObFTCharUtil::classify_first_char(coll, "A", 1, type),
            "unsupported charset return code");
    require(type == CharType::CHINESE, "unsupported charset preserves output parameter");
    compare(coll, "A", 1);
    compare(coll, nullptr, 0);
  }
  std::cout << "PASS: " << comparisons << " frozen-baseline classification comparisons\n";
}

static void check_decode_budget()
{
  struct Fixture { const char *text; uint8_t n; uint64_t decodes; };
  const Fixture cases[] = {{"A", 1, 0}, {"9", 1, 0}, {"\xe4\xb8\xad", 3, 1}, {" ", 1, 0},
                           {"\xe3\x81\x82", 3, 1}, {"\xf0\x9f\x98\x80", 4, 1}};
  for (const auto &f : cases) {
    CharType type = CharType::USELESS;
    decode_calls = 0;
    require(OB_SUCCESS == ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, f.text, f.n, type),
            "decode budget fixture return code");
    if (decode_calls != f.decodes) {
      std::cerr << "observed decoder calls=" << decode_calls << " expected=" << f.decodes << '\n';
      require(false, "ASCII needs no decoder call; other characters need at most one");
    }
  }
  std::cout << "PASS: zero ASCII decoder calls and one per non-ASCII classification\n";
}

int main(int argc, char **argv)
{
  check_literal_categories();
  check_equivalence();
  if (argc < 2 || std::string(argv[1]) != "--equivalence-only") {
    check_decode_budget();
  }
}
