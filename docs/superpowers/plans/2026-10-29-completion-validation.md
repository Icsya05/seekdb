# Completion configuration and response validation

**Goal:** Honor optional SQL NULL and native JSON object configuration, and reject empty/non-string OpenAI completion content.

**Spec:** Official AI function enhancement task, locally saved `output/research/prelim-2026/psu9fks8dexldoum.txt`; AI_COMPLETE requires NULL config, JSON config, and nonempty responses. User authorized autonomous implementation.

**Architecture:** Keep configuration conversion in the SQL evaluator. Skip explicit NULL; decode native JSON with the existing expression helper and require an object; preserve text/LOB JSON parsing. Keep response validation inside the OpenAI completion provider, matching the existing Ollama string contract. No API or official test assets change.

**Files:** `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp`, `ob_ai_func_utils.cpp`; focused actual-function boundary harness under `unittest/sql/engine/expr/` and opt-in SQL tests beside existing AI regression tests.

**Review focus:** Omitted config, explicit NULL, native object versus array/scalar, string JSON/LOB, missing/empty/non-string completion output, valid Unicode text, stale output pointer after failure. Boundary doubles are not full-server verification.

- [x] Add focused behavior tests and observe baseline failures: config two failures; parser four failures.
- [x] Apply minimal evaluator/provider fixes; observe focused GREEN: seven config and five response cases.
- [x] Compile affected production translation units with fixed real CMake flags; both actual objects exit0. Combined discovery:33passed,22SQLskipped,55total.
- [x] Add opt-in real SQL coverage; released server2883 reproduced the four broken behaviors (NULL/nativeJSON config, empty/nonstring completion). Three controls passed. This is RED evidence, not verification of modified SQL.
- [x] Obtain independent review: no Major/Blocker; record evidence/limits.
- [ ] Run seven SQL cases against the modified full-server build when available.
- [ ] Commit and push verified work without competition submission.
