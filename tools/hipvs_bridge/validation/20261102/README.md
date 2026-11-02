# AMD SQL development validation — 2026-11-02

This is a development report, not an official competition result. `trial80` caps NN-Descent at 80 iterations; `candidate` is the workspace-only implementation at 100 iterations; `baseline` is frozen V5 bridge code. Search still calls the installed hipVS CAGRA implementation. FP32 inputs, L2, graph degree 128 and intermediate degree 256 are unchanged.

## Provenance and method

- Frozen V5: `634888d9b037153fa143d2c913ba4b25503e1d62`.
- Workspace-only source: `dbece630f97d1e87908ccd863b013bd276d89ce4`. `git merge-base --is-ancestor` passed. All C++ server sources are unchanged from V5; only bridge implementation/build/header and tests/docs differ.
- Full server compiled on the competition image with official RelWithDebInfo/unity/LLD/CUVS ON/TRACE OFF flags and offline Cargo. Rust 1.97.1 and frozen dependency cache were used. Bridge libraries were compiled with the official bridge script. The same server binary loaded each bridge variant; `/proc/<pid>/maps` verified the selected path.
- Full server SHA256: `12536f254fd40e80b8cbef2da7773433277f3b9d2c98066125e1bd3da6972839`.
- The source archive had no `.git`; the binary's generated revision string is not the provenance proof. Use the recorded archive hash and source commit. Official evaluation must compile from a complete Git checkout.
- ROCm 7.2.4/gfx1100 competition environment. Cgroup CPU quota was 16 cores, although server `cpu_count=24`; this differs from the official 24-core runtime allocation. Server memory budget 8G, datafile 8G. No concurrent benchmark servers were run.
- Public SIFT: first 250000 train rows and first 4096 test rows of the pinned public HDF5 file; SHA256 `dd6f0a6ed6b7ebb8934680f861a33ed01ff33991eaee4fd60914d854a0ca5984`. Top-100 truth was recomputed over the selected 250K, not copied from the original 1M truth. This row/query selection has not been verified to equal the official selection.
- Gaussian: independent FP32 base/query vectors, NumPy seed 20261357, shapes 250000×256 and 4096×256. Exact L2 reference uses FP64 CPU accumulation of FP32 inputs. This is legitimate development data, not Hidden data.
- Data load is outside construction timing. Time begins at offline `CREATE VECTOR INDEX` and ends after the first fixed Top-10 SQL query completes. It is not isolated NN-Descent or kernel time.
- Each query phase executes 4096 sequential single-row SQL statements at Top-10 and Top-100. Every result is checked for exactly k unique IDs in range. PyMySQL client timing includes retrieval and integrity-check overhead.
- One run/order per variant: SIFT baseline→candidate→trial80; Gaussian trial80→baseline→candidate. No statistical repeatability claim. Driver scripts and comparisons are included here.

## Paired GPU results

| Dataset | Variant | DDL through first query (s) | Recall@10 | Recall@100 | QPS@10 | QPS@100 |
|---|---|---:|---:|---:|---:|---:|
| Public SIFT250K | V5 baseline | 45.924 | 0.999951 | 0.999961 | 137.15 | 131.26 |
| Public SIFT250K | workspace / 100 | 46.276 | 0.999976 | 0.999963 | 140.00 | 135.06 |
| Public SIFT250K | workspace / 80 | 40.344 | 0.999976 | 0.999976 | 141.25 | 135.94 |
| Gaussian256250K | V5 baseline | 45.762 | 0.950659 | 0.898760 | 121.17 | 117.52 |
| Gaussian256250K | workspace / 100 | 45.850 | 0.950439 | 0.899185 | 126.21 | 122.33 |
| Gaussian256250K | workspace / 80 | 40.650 | 0.948999 | 0.896877 | 127.68 | 122.12 |

Trial80 reduces measured DDL-through-first-query time by **12.15% on SIFT** and **11.17% on Gaussian256** (speedup factors about 1.138× and 1.126×). The workspace-only candidate has essentially unchanged construction time; the observed construction improvement belongs to the iteration-cap experiment.

Trial80 Gaussian Recall decreases by **0.1660 percentage points @10** and **0.1882 percentage points @100** from this run's baseline. These declines are within the 0.5 percentage point development comparison, but cannot guarantee Hidden quality. The host contract assertion of 80 checks parameter propagation only.

All measured variant pairs satisfy the corresponding development Recall absolute floors/baseline deltas and QPS ratio≥0.9. Recorded allocated VRAM peaks are about 734MB (SIFT) and 862MB (Gaussian256), with variant/baseline ratios approximately 1.000. Sampling reads global DRM allocated VRAM every 20ms, can miss short transients, and is not the official resource sampler. Do not report an official memory-gate pass from this evidence alone.

## Lifecycle and CPU checks

Both workspace/100 and trial80 passed a 10K×128 SQL lifecycle diagnostic: load before DDL, cuvs and vsag index construction, five queries per backend with minimum Recall@10 0.9, and ten before/after-restart result-set comparisons. Server logs show actual GPU CAGRA snapshot serialization with `built=true` and deserialization after restart. This is a small-scale persistence diagnostic, not a complete formal gate suite.

Linux LP64 and Windows strict C11/C++17 host suites pass allocation reuse, default parameter handling, failure recovery, invalid inputs and concurrent search/serialization. Windows explicitly skips the LP64 overflow fixture; Linux executes it.

On the same public SIFT250K subset/configuration, CPU VSAG measured 81.349s through first query, versus 40.344s for trial80 GPU (about 2.016×). CPU Recall@10/@100 was 0.980957/0.949780 and QPS was 3333.27/1395.09. GPU Recall was higher and single-row SQL queries were slower; this CPU/GPU table is descriptive, not a quality-matched replacement for the official frozen-GPU comparison.

The instance-wide cgroup memory peak was 30,347,038,720 bytes across compilation and tests; its memory limit was 59,055,800,320 bytes and recorded OOM counters were zero. This is broader than server RSS and is not the official sampler. Installed libcuvs_c.so SHA256 was `b63211897276e0a6911bb534fe77c8b0b59f31c50af1df2f108845f06ad46ebf`.
## Limits and submission choice

No Hidden evaluation or official score exists for these candidates. Exact installed hipVS source commit could not be independently proven from image metadata; its library hash is recorded in local evidence. Fixed library files were not modified. Runtime pairing is meaningful for development but cannot replace the official clean build, same-round baseline and all hard gates.

Use `competition/amd-search-workspace` for the conservative 100-iteration option. This `competition/amd-nnd80` branch is an explicitly experimental alternative with measured development improvement and a quality tradeoff. Both descend from frozen V5. The repository owner must make the answer repository private before final evaluation as required by the contest; never put a token in source or reports.

## Reproduction on the competition image

The diagnostic scripts intentionally retain the exact paths from the recorded experiment. Use a fresh instance; existing runtime/output directories cause refusal instead of deletion.

1. Put a complete V5-descendant checkout at `/workspace/amd-full-dbece630/seekdb`. Set `PATH=/usr/local/bin:$PATH` and `CARGO_NET_OFFLINE=true`. Link `deps/3rd` to the image's frozen `/opt/seekdb-deps-cache/ac032efc6b707fc0273e6b553db53011/3rd` if absent.
2. Create `/workspace/seekdb_hipvs_notebook`, `/workspace/amd-baseline`, `/workspace/amd-trial80`. Copy the Python files from this directory into the Notebook directory.
3. Extract baseline `tools/hipvs_bridge/{build.sh,seekdb_cuvs_bridge.c,seekdb_cuvs_bridge.h}` with `git show 634888d9b037153fa143d2c913ba4b25503e1d62:<path>` into a separate source directory. Similarly extract the workspace/100 files from `dbece630f97d1e87908ccd863b013bd276d89ce4`. Keep LF endings. Build baseline to `/workspace/amd-baseline/libseekdb_cuvs_bridge.so`, workspace/100 to `/workspace/amd-full-dbece630/libseekdb_cuvs_bridge.so`, and this branch's bridge to `/workspace/amd-trial80/libseekdb_cuvs_bridge.so`. Use `HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash <variant>/build.sh <output>`; never modify `/opt/hipvs`.
4. Build the server from the checkout using the official commands below. The unchanged C++ server loads variant libraries by SONAME and `LD_LIBRARY_PATH`; each driver verifies the actual loaded path.
5. Only for development data/client helpers, install h5py and PyMySQL into `/workspace/amd-devenv` with `python -m pip install --no-deps --target /workspace/amd-devenv h5py PyMySQL`. The image supplies NumPy; this does not alter the frozen server dependencies.
6. Download the public file to `/workspace/seekdb_hipvs_notebook/sift-mirror.hdf5` from `https://huggingface.co/datasets/hhy3/ann-datasets/resolve/0bc1ed67fc2626bc6f110c8794a70572681b7395/sift-128-euclidean.hdf5`. `prepare_sift250k_dev.py` verifies its SHA256 before processing. Set `PYTHONPATH=/workspace/amd-devenv OPENBLAS_NUM_THREADS=2 OMP_NUM_THREADS=2`, then run both preparation scripts from the Notebook directory.
7. Run `python paired_vector_sql_dev.py <baseline|candidate|trial80> --dataset <sift|gaussian256>` sequentially, with no competing GPU work. Each uses port2992 and distinct output/runtime directories; do not start variants in parallel. Preserve summary.json, result ID arrays, process maps, metadata and server logs. Inspect failed summaries; process exit alone is not a pass signal.
8. Run `python sql_lifecycle_diagnostic.py` and `python sql_lifecycle_trial80.py` sequentially for the small restart checks (port2991). `paired_cpu_sql_dev.py cpu --dataset sift` provides a separate CPU VSAG comparison using the same public data/configuration.

Official server build flags used:

```sh
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/workspace/amd-full-dbece630/libseekdb_cuvs_bridge.so
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build_release --target seekdb
```

For official submission compile the branch's own bridge and pass that output path, rather than the development pairing's workspace/100 library. The official grader recompiles from Git; these experiments do not authorize substituting precompiled artifacts.