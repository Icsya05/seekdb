# AMD CAGRA reusable search workspace implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Avoid per-query GPU allocations without changing CAGRA results or tuning parameters.
**Architecture:** Each opaque bridge handle owns reusable query/result device buffers and one search-parameter object. A per-handle mutex serializes search and serialization. Growth is transactional and teardown frees all retained resources.
**Tech Stack:** C-compatible bridge, HIP/hipVS C API, POSIX mutex, C++ host contract test.
**Spec:** D:/OceanBase-AMD/research/2026-11-01/official-amd-observation.txt (AMD V5 official contest requirements).

## Global Constraints
- Base: 634888d9b037153fa143d2c913ba4b25503e1d62 (official V5).
- Fixed hipVS: f96a663d0a4441d6f5344bd3ef552b71ae7b370b; do not edit it.
- Actual search remains cuvsCagraSearch; retain FP32/L2 and all current construction/search settings.
- Preserve public C ABI, OBCUVS02 persistence, CPU VSAG behavior, build output paths.
- Local host contract tests do not establish GPU correctness, recall, QPS or contest score.
- All artifacts stay on D:; no full ROCm build is available on this Windows host.

## Review Focus
- Allocation failure while growing: old workspace remains usable, no leaked allocations.
- Invalid/overflow dimensions: reject before a device API call.
- Output copy failure: report failure, allow next call to succeed.
- Concurrent same-handle operations: no shared workspace races; free requires exclusive ownership.
- Deserialized handles: initialize and reclaim the same workspace as fresh builds.

### Task 1: reusable search workspace
**Files:** Modify tools/hipvs_bridge/seekdb_cuvs_bridge.c, seekdb_cuvs_bridge.h and build.sh; create tools/hipvs_bridge/tests/test_search_workspace.cpp, narrowly scoped host API doubles and runners.
**Interfaces:** Existing seekdb_cuvs_build/search/serialize/deserialize/free signatures remain unchanged.
- [x] Write a host contract test compiling the real bridge against CPU-backed boundary doubles; assert repeated search output, allocation stability, shape growth, failure recovery, cleanup and concurrent calls.
- [x] Run test against V5 and record an expected allocation-reuse failure.
- [x] Add per-handle workspace, lock and size checks; preserve CAGRA parameters and fixed API search.
- [x] Run all bridge host cases, strict C/C++ compilation and diff checks.
- [x] Record limits and ROCm persistence/paired benchmark requirements in tests/README.md; commit reviewed local candidate, never claim measured GPU improvement.

## Execution ledger
- Ruling: Use the already dedicated D: clone and a new feature branch instead of another worktree; no shared/uncommitted work exists here, and the user requested all artifacts on D:.
- Ruling: Execute directly under the user's existing authorization to continue; no additional design approval checkpoint.
- Pre-flight: one task, no inter-task interfaces.

- RED: V5 failed `reuse` at alloc_calls==initial_allocs before production changes.
- GREEN: C11 and C++17 host suites passed; suite expanded to nine cases after review.
- Review: fresh reviewer found no critical/important production defect. Minor findings: serialize overlap coverage, Windows overflow skip, ignored free failure.
- Fix RED: free_failure failed at f.search(100)!=0, then reserve retained the old pointer on release failure; full suite passed.
- Mutation check: a D: scratch copy without the serialize lock failed concurrent_serialization, proving the new overlap test detects the missing lock.
- Ruling: LP64 overflow fixture cannot be represented by Windows 32-bit long with this size_t; visibly skip and require Linux run rather than claim coverage.
- API check: fixed hipVS public C headers + DLPack v0.8 compiled C11/C++17; host CUDA type shim only, no actual ROCm compilation/link/runtime.
- Pending: actual ROCm build, persistence smoke, SQL/VSAG gates, paired GPU recall/QPS/build/VRAM benchmarks. Do not submit as a validated performance result yet.

- Final verification: PowerShell and bash runners both passed nine cases in C11 and C++17; expected injected errors and explicit Windows overflow SKIP recorded. Pinned-header C/C++ syntax and production build.sh syntax passed; git diff --check passed.
- Cloud reconnaissance: correct portal is https://developer.amd.com.cn/radeon/ (trailing slash); seekdb-hipvs template 3102 is available. Preview describes an older demo, so validate fixed V5/hipVS versions in a real instance. No instance launched or credits consumed. Browser session returned/closed.
