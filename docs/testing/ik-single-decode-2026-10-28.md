# IK single-decode verification

Base: upstream `2026-competition`, `5bce9e06d1281347ad61344e4b01fab9158c58a2`.

`ObFTCharUtil::do_classify` previously decoded the same input separately for
each predicate: twice for digits, three times for Chinese and four times for
punctuation or other CJK. It now decodes once and applies the same predicates
in the same order. UTF-16 surrogate predicates retain their charset guard.
No dictionary, tokenizer rule, SQL, benchmark or score baseline changes.

## Executed checks

- Before the production change, the focused test passed **1,309,594** comparisons
  against the frozen original header, then failed the decoder budget:
  `observed decoder calls=2 expected=1` (digit fixture).
- After the change, the same comparisons and one-decode fixtures passed with
  Windows MinGW GCC **15.2.0**, both `-O0` and `-O2`.
- The focused suite also passed on Ubuntu 24.04 / GCC **13.3.0**, `-O2`.
- The actual production header, with **no platform shim**, passed Linux
  `g++ -std=gnu++17 -fsyntax-only -I. -Isrc -Isrc/oblib -Ideps/oblib/src`.
  A strict `-std=c++17` probe initially failed in existing variadic serialization
  macros; the project-compatible GNU dialect passed.
- `git diff --check` passed.

The comparisons cover every Unicode scalar encoded as UTF-8, all one-byte and
two-byte sequences under UTF-8/UTF-16/UTF-16LE, representative malformed longer
UTF-8, truncation, null pointers, zero lengths, and unsupported GBK, GB18030,
Latin1, binary and invalid collations. Literal fixtures independently pin
fullwidth letter/digit precedence, Chinese, Japanese, Korean, ASCII, spaces
and supplementary emoji categories.

## Reproduction

From the repository root, with Python 3, Git, C++17 and the base commit available:

```bash
python3 unittest/storage/fts/run_ik_char_util_test.py --cxx g++
python3 unittest/storage/fts/run_ik_char_util_test.py --cxx g++ --optimization 0
```

When Linux Python operates on a Windows-created Git worktree under WSL, use
`--git /mnt/c/path/to/git.exe`; native Linux Git cannot resolve the Windows
absolute paths recorded in that worktree's `.git` file.

The runner compiles the **actual production IK header**, the actual charset
helper and `mb_wc.h` UTF-8 decoder, and the actual `unicode_utils.cpp`. The frozen
header is obtained with `git show`, renamed only to coexist in the test process.
The temporary shim supplies logging macros, charset dispatch, primitive type
declarations and unused string-conversion declarations. It does not implement
classification, Unicode predicates or decoding. Dispatch integration and logging
side effects are therefore outside this focused test's coverage.

The compiler emits existing signedness, unused-variable/parameter and intentional
fallthrough warnings from the included baseline/header/Unicode utility sources.
The shim test is not a warning-clean full database build.

## Remaining validation

No seekdb binary was built or started for this change. Existing FTS mysqltests,
TOKENIZE/MATCH integration and the complete official before/after benchmark
have **not run**. Decoder counts demonstrate eliminated repeated work in the
instrumented header; they do not establish elapsed-time improvement after
production compiler optimization and do not establish a competition score.

Once the normal Linux toolchain and binary are ready, build both versions with
the same settings, run the existing `fts_index` tests (especially
`tokenize_function`), then run the unchanged official benchmark in a dedicated
instance with fresh report paths:

```bash
LABEL=before SKIP_LOAD=0 OUTPUT=/tmp/fts-before.txt bash tools/benchmark/fts_large_bench.sh
# Rebuild/restart with the optimized binary, keeping hardware and settings equal.
LABEL=after SKIP_LOAD=0 OUTPUT=/tmp/fts-after.txt bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/fts-after.txt --json
```

Use all official default parameters (20,000 rows, batch 500, 3,000 tokenize
rounds, 200 query rounds, 3 samples, 30 warmups) and compare all nine timed
metrics and query results. The benchmark deletes/recreates `fts_large_bench`.
The task description references `.github/workflows/vldb.yml`, but that path
does not exist in this frozen checkout; no such CI result is claimed here.
