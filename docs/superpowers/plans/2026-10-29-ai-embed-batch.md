# AI_EMBED Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute compatible active AI_EMBED rows in one provider request and preserve SQL row identity.

**Architecture:** Register an engine batch callback. Evaluate vector/scalar arguments through datum vectors, validate active rows before network calls, group by model and dimension, and copy returned strings into each row's result memory. Reuse the existing provider parser and embedding vector API.

**Tech Stack:** seekdb C++ expression engine; Python unittest production-function harness and opt-in PyMySQL/HTTP regression tests.

**Spec:** `output/research/prelim-2026/psu9fks8dexldoum.txt` in the parent project.

## Global Constraints

- 基于 seekdb SQL 引擎实现，不得在应用层绕过。
- 不得针对模型名、输入或响应硬编码。
- 异常输入或网络错误不得导致崩溃、结果错位或资源泄漏。
- Leave official mysqltest/data/dependencies unchanged.

## Review Focus

- NULL content never enters provider inputs, including all-NULL batches.
- Filtered and already evaluated rows never issue requests or change results.
- Varying models and dimensions never share a request.
- Invalid dimensions abort before requests; provider failures leave no completed results for that request.
- Multiple engine batches and row result allocation do not overwrite earlier results.

### Task 1: Engine batch callback and regression coverage

**Files:** modify `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.{cpp,h}`; create `unittest/sql/engine/expr/test_embed_batch.py`, `embed_batch_harness.cpp`, `test_embed_batch_sql.py`.

**Interfaces:** consumes `ObExpr::eval_batch`, `ObDatumVector`, `ObAIFuncModel::call_dense_embedding_vector_v2`; produces `ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &, ObEvalCtx &, const ObBitVector &, int64_t)`.

- [x] Write and run a production callback harness: compatible rows produce one multi-input call and mapped per-row outputs; baseline must fail because no callback exists.
- [x] Implement callback, register it, and run harness cases for NULL, skip, evaluated flags, scalar args, model/dimension groups, invalid dimensions, failure, count mismatch, LOB reads, and repeated batches.
- [x] Add opt-in real SQL local HTTP tests for scan batching, out-of-order responses, nullable rows, filter, LIMIT, multiple batches, grouping, and failure.
- [x] Run existing expression Python suite and compile production translation unit with actual native flags.
- [ ] Commit source, tests, and this plan; report SQL/native verification limits honestly.

## Execution ledger

- Ruling: Continue autonomously using the explicitly supplied task and execution method; root coordinates final review and native SQL integration.
- 2026-10-29: Production-function harness RED 15/15 missing callback failures, then GREEN 15/15 with UBSan. Existing expression Python discovery GREEN 99 tests, 41 SQL opt-in skips. Production translation unit compiled to a native object using actual native build flags, exit 0.
- 2026-10-29: Opt-in SQL suite against the preserved old binary on port 2883 was RED: scan issued eight singleton requests, model/dimension grouping issued eight requests, filter issued four singleton requests, multi-batch inputs remained singleton. NULL tests hit the baseline NULL rejection. Invalid-dimension test observed earlier HTTP requests. Full new-binary SQL validation is coordinated by the parent after integration and a combined incremental build.
- Ruling: HTTP failures may use the existing client's retries; SQL failure tests require every retry to carry the same whole compatible batch rather than asserting a single transport attempt.
