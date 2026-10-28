# IK character classification implementation plan

> **For agentic workers:** Execute inline with superpowers:executing-plans. The parent agent performs the final review.

**Goal:** Decode each IK character once while preserving classification and error behavior.

**Architecture:** Keep the existing charset dispatch and ordered Unicode predicates. Reuse the decoded codepoint within `ObFTCharUtil::do_classify`; keep UTF-16 surrogate handling restricted to its existing encodings.

**Tech Stack:** C++17, Python 3 standalone unit-test runner, existing charset decoder and Unicode utilities.

**Spec:** Official fulltext optimization task (`za7cfo5cazcrihhq`, content updated 2026-10-26) and upstream `2026-competition` commit `5bce9e06d1281347ad61344e4b01fab9158c58a2`.

## Global constraints

- Preserve TOKENIZE and MATCH semantics; do not modify benchmark inputs, SQL, timing, scoring or baseline.
- Work only on `competition/fts-20261028`; no push, PR or merge during this implementation.
- Treat dependency setup failures as environment failures, never as passing database tests.

## Review focus

- Preserve priority for fullwidth Latin/digits overlapping the other-CJK block.
- Preserve invalid/truncated input and null-pointer return codes.
- Preserve current UTF-16 and UTF-16LE decoder behavior rather than introduce unrelated charset fixes.
- Preserve unsupported-charset behavior, including GBK, GB18030 and Latin1.
- Do not claim measured database speedup from a decoder-call count reduction.

## Task: single-decode classification

Files: `src/storage/fts/ik/ob_ik_char_util.h`; standalone tests and runner under `unittest/storage/fts/`; verification report under `docs/testing/`.

- [x] Add tests that include the real production header, charset helper, UTF-8 decoder and Unicode implementation. Restrict doubles to platform/logging scaffolding.
- [x] Run the decoder-budget test against unchanged production code; observed failure on the digit fixture (two calls rather than one).
- [x] Replace repeated per-predicate decoding with one decode and the unchanged predicate order; preserve charset guards.
- [x] Verify every Unicode scalar value, all 1- and 2-byte input combinations, representative malformed longer UTF-8, UTF-16 dispatch and unsupported charsets against the frozen original header.
- [x] Run the focused suite with optimization enabled and disabled, inspect the diff and record exact limitations.
- [ ] If a real built seekdb becomes available, run existing FTS correctness tests and the unchanged complete official benchmark before/after. Otherwise leave this explicitly unverified.

## Execution ledger

- Investigation: the original `do_classify` invokes the same decoder through each predicate: three times for Chinese, four for punctuation, up to six for UTF-16. The official competition branch contains the same code.
- Ruling: the public checkout omits module unit-test build files. Add a standalone test inside `unittest/storage/fts`, using actual production headers and a temporary platform shim, rather than claim a nonexistent module target passes.
- Completed: 1,309,594 equivalence comparisons and decoder-budget fixtures pass on GCC 15.2.0 MinGW (-O0/-O2) and GCC 13.3.0 Linux (-O2). Real Linux production headers also pass syntax checking without shims. Details and unrun server/benchmark checks are in `docs/testing/ik-single-decode-2026-10-28.md`.
