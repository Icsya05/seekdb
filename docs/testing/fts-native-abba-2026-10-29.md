# Native IK / full-text validation, 2026-10-29

Matched full production binaries were built with the same pinned Clang 17
RelWithDebInfo configuration. Control `8eeee63ee` differs from optimized
`140f2f14d` only in `src/storage/fts/ik/ob_ik_char_util.h`: the control restores
the exact original header from official competition commit `5bce9e06d`.
Other AI and storage code is identical between these two binaries.

## Method and correctness

Both binaries used the same isolated WSL instance and directory, cpu_count=2,
memory_limit=1G, datafile_size=512M, datafile_maxsize=2G, log_disk_size=4G.
The unchanged official benchmark ran in balanced ABBA order (control, optimized,
optimized, control). Each run restarted the instance and used all defaults:
20,000 rows, batch 500, 3,000 tokenize rounds, 200 query rounds, three samples,
30 warmups and SKIP_LOAD=0. All four runs exited 0. Heavy project compilation
was paused during measurement; the host was not an isolated benchmark machine.

All four runs produced identical complete official TOKENIZE test outputs,
additional runtime IK corpus outputs, full MATCH ID sets and ordered LIMIT 20
IDs. Full predicate counts were 8,001 Chinese, 11,000 BENG, 7,332 mixed and 9,333
for the predicate used by the LIMIT query. The comparison checks IDs, not only
counts. The previous exhaustive character-classification regression remains
separate coverage of the modified helper.

## Retained measurements

The table averages both complete runs on each side, retaining every measured
sample. Positive percentages mean lower elapsed time. Units follow each metric.

| Metric | Control mean | Optimized mean | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 12.530000 | 12.648000 | -0.94% |
| build_ik_content_sec | 9.540500 | 9.040500 | +5.24% |
| build_beng_en_sec | 5.609000 | 5.602500 | +0.12% |
| tokenize_ik_avg_ms | 0.339150 | 0.277850 | +18.07% |
| tokenize_beng_avg_ms | 0.248700 | 0.222600 | +10.49% |
| query_cn_avg_ms | 2.039750 | 2.655300 | -30.18% |
| query_beng_avg_ms | 2.747300 | 3.364900 | -22.48% |
| query_mixed_avg_ms | 2.136650 | 2.098950 | +1.76% |
| query_limit_avg_ms | 3.116050 | 2.473850 | +20.61% |

Results are mixed. The IK tokenize mean is lower, while Chinese and BENG query
means are higher. SELECT 1 drifted about 9% between aggregate sides and the two
control runs varied substantially. Unmodified BENG also varied, so these local
numbers do not establish a uniform or causal database-wide speedup. They are not
a competition score and do not justify extrapolating component microbenchmarks
to full-query improvement.

Raw run metrics, binary SHA-256 values, parameter values, complete comparison
aggregates and correctness hashes are retained in
[the machine-readable report](fts-native-abba-2026-10-29.json). No official
benchmark, SQL, data generator or scoring baseline was changed.

## Reproduction

Build optimized commit 140f2f14d normally. In a separate control worktree based
on that commit, restore only the IK header from 5bce9e06d and build with identical
dependencies and flags. Verify the source diff and each binary version/SHA.
Restart an isolated server with the same configuration for each run, then invoke
the unchanged tools/benchmark/fts_large_bench.sh with fresh OUTPUT files and all
default parameters. Alternate ABBA order and retain every result. Do not use
the earlier ebeffd6b checkpoint as a pre-optimization control: it already contains
the optimized classifier.
