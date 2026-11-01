# CAGRA bridge host contract tests

These tests compile the **real production bridge** with CPU-backed doubles at the
external cuVS/CUDA API boundary. They check workspace ownership, copies, returned
IDs/distances, parameter preservation, concurrency and injected-error recovery.
They do not execute CAGRA, emulate GPU kernels, measure GPU speed, validate GPU
persistence or establish contest recall/score. The build script for the production
library never includes `tests/include`.

## Run

Windows (all artifacts on D:):

```powershell
./tools/hipvs_bridge/tests/run_host_tests.ps1 `
  -BuildDir D:/OceanBase-AMD/build/bridge-tests `
  -Cc E:/develop/minGW/w64devkit/bin/gcc.exe `
  -Cxx E:/develop/minGW/w64devkit/bin/g++.exe
```

Linux:

```bash
bash tools/hipvs_bridge/tests/run_host_tests.sh /tmp/seekdb-bridge-tests
```

The runner builds the bridge both as C11 and as C++17 (hipcc's language mode),
using `-Wall -Wextra -Werror -pthread`. Expected: nine PASS cases per build;
injected failures also deliberately emit bridge error messages on stderr.
On Windows LLP64 the large-shape overflow fixture prints an explicit SKIP:
run it on LP64 Linux to cover that branch. C API signatures and consumed fields
were checked against hipVS `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`.
The test declarations model only consumed fields, not the SDK ABI/layout.

## Cases

- `reuse`: repeat identical queries without additional device allocations or
  parameter creation; grow for Top-100 and two-query batches; shrink Top-K again.
- `library_default`: keep a library default larger than the bridge's minimum.
- `failure_recovery`: failures at all three allocation/copy stages and search;
  a subsequent small query still succeeds and final teardown reclaims allocations.
- `free_failure`: failed old-buffer release retains ownership and returns failure.
- `parameter_failure`: failed lazy parameter creation can be retried.
- `invalid_inputs`: invalid pointers, empty/negative counts, excessive Top-K,
  overflow-capable shapes (LP64) and optional distance output.
- `concurrent_search`: four threads use one handle with different Top-K values.
- `concurrent_serialization`: search and serialize cannot overlap on one handle.
- `restored_workspace`: deserialized handles reuse and free their workspace.

The fake search computes CPU exact L2 on a tiny hand-checkable fixture solely to
make copy/output-buffer mistakes visible; production search is unchanged and still
calls the fixed hipVS CAGRA implementation.

## Real GPU checks still required

1. On the competition image confirm ROCm 7.2.4 and fixed hipVS commit. Compile with
   `tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so` using the
   official seekdb compilation procedure, not the test include directory.
2. Run the existing `test_persistence.cpp` write/read smoke against the actual
   bridge/hipVS, plus the existing offline-DDL persistence smoke in
   `docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_persistence_smoke.sh`.
3. Run baseline/candidate paired single-SQL-query benchmarks, Top-10 and Top-100,
   for both 250K x 128 and 250K x 256 input shapes; collect build+first-query
   wall time, recall, QPS and peak GPU memory. Follow the published 4096-query
   scoring protocol rather than batch_knn. No measured speedup is claimed yet.
4. Verify unchanged CPU `lib=vsag` operations and post-restart GPU serving before
   selecting any candidate for submission.

Free requires exclusive handle ownership. Search/serialize serialize internally;
retained allocations last until handle destruction and grow only as needed.
For a single query with dim=256 and Top-100, user buffer capacity is 1824 bytes
(excluding hipVS/RMM overhead). Build parameters remain NN_DESCENT, 128/256 graph
degrees and 100 iterations; search preserves the original default/minimum rules.

## Persistence smoke control flow

The runners also compile test_persistence_host.cpp, which reuses the host API boundary and executes the actual persistence smoke's write mode. It checks the success path, serialization failure cleanup (exit 3), copy failure cleanup (exit 5), and invalid mode rejection (exit 2). This does not read or write a real CAGRA snapshot and is not a GPU persistence test. The actual smoke now alternates Top-K and query batch sizes, validating finite/nonnegative distances, unique in-range IDs and self matches after each search.
