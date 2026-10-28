# IK classification diagnostic, 2026-10-28

This bundle preserves uninstrumented component microbenchmarks, including the
initial ASCII regression. It does not contain the official competition benchmark
and does not report database performance or competition scores.

- `single-decode-no-ascii-results.json`: first implementation, local commit
  `20e688607`; six balanced AB/BA pairs per corpus/optimization. ASCII -O2 regressed.
- `single-decode-ascii-results.json`: first fast-path run, same methodology.
- `single-decode-ascii-repeat-results.json`: repeated fast-path run, including
  compiler, corpora, source header SHA-256 and method metadata.
- `microbenchmark-summary.json`: medians from every retained pair; no samples
  removed. `summarize_microbenchmark.py` regenerates it.

Run on Linux, with the competition checkout and its baseline commit available:

```bash
python3 microbenchmark.py --repo /path/to/seekdb-fts-competition \
  --cxx /usr/bin/g++ --output /tmp/ik-diagnostic.json
python3 summarize_microbenchmark.py
```

For a Windows-created worktree accessed through WSL, also pass
`--git /mnt/c/path/to/git.exe`. The runner reuses the focused test's temporary
logging/platform/dispatch shim, while compiling the actual IK production header,
charset helper, UTF-8 decoder and Unicode predicates. The counter is disabled.
The runtime corpus/character order and consumed checksum prevent loop hoisting
without `noinline`, `volatile` or assembly barriers. Every measured old/new pair
must return the same checksum or the executable fails.

Measurements use GCC 13.3.0, -O2/-O3; the pinned Clang17 dependency extraction was
still incomplete. Production and complete-database results remain unmeasured.
