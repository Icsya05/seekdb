# AI Column implementation and validation notes

This branch extends the native SQL parser, schema, resolver and executor with
asynchronous AI columns. It includes the endpoint and File SQL branches as
prerequisites. It has not been submitted to the competition judge.

## SQL contract

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  content TEXT,
  tags JSON AI COLUMN (AI_COMPLETE('completion_model', content))
    WITH (sync_mode='async', max_retries=2),
  embedding VECTOR(4) AI COLUMN (AI_EMBED('embedding_model', content))
);
ALTER TABLE docs RETRY AI COLUMN tags WHERE status='FAILED';
SET SESSION ai_column_strict_read=ON;
```

Models and endpoints must already be registered. DDL resolves the expression
without making a model request. AI results are nullable physical columns.
Explicit user assignments to AI result columns and their internal state are
rejected. Input expressions must be deterministic and cannot depend on AI or
generated columns, subqueries, session variables or another nested AI call.

The retry-only, unquoted `status` name identifies the selected AI column's
status. A backtick-quoted `status` identifies the business column. Retry is
transactional DML: its invalidations and newly queued work roll back together.

## Transactions and worker ownership

The durable task queue resides in hidden fields of the business row. Insert,
dependency update, primary key update and delete therefore use the same native
MVCC transaction as the corresponding task change. Updating an unrelated
column preserves its AI results. Vector index maintenance receives the NULL
invalidation as part of the native update.

The fields record status, request identity, owner, lease expiry, attempt count,
definition version, eligibility time, retry count and task state. Members
created by one row change share a request UUID; different rows use different
UUIDs. A manual retry replaces only the selected column's request.

Four background workers scan committed rows and claim eligible work using
conditional updates. Each model request runs outside a business transaction.
Attempt counts and retry eligibility are committed before another attempt.
Ownership leases expire after 60 seconds and model SQL has a 30-second timeout.

The worker validates all selected results before publishing the group's
terminal values and statuses in one update. Writeback checks the primary key,
request UUID, owner and current schema definition. A response from deleted,
updated, retried or redefined work cannot overwrite a newer request. A final
failure leaves the result NULL and marks the AI column FAILED.

## Typed requests and public catalogs

The normalizer derives a model expression from the persisted AI expression and
declared SQL type. Completion requests preserve the evaluated user input and
carry the type instruction as a system message. Embedding requests carry the
declared vector dimension. Returned values are validated before publication.

`oceanbase.__all_ai_column` exposes definitions and the actual normalized
expression used by the worker. `state_column_id` and `request_column_id` refer
to their hidden fields; `result_spec_column_id` identifies the hidden version
of the result definition. `oceanbase.__all_ai_column_task` exposes pending work,
including row key predicates, eligibility, ownership, attempts and retries.
Task states are 1 (pending), 2 (running) and 3 (waiting to retry).

These catalogs expand to native SELECTs over schema definitions and hidden
queue fields. They are not independent copies of the queue. Task scans use the
caller's transaction, so an uncommitted task is visible to its creator and
disappears on rollback. Expansion disables plan caching because its contributing
tables can change on CREATE, DROP or ALTER. The present expansion scans all AI
tables; a large catalog needs additional pruning and performance validation.

## Strict reads

The resolver records referenced base AI columns before query rewrites. Execution
checks the session variable on every use of a cached plan. In strict mode, a
referenced column with a pending or failed row raises error 11120 even when the
outer predicate selects only READY rows or no rows. Ordinary columns and other
READY AI columns remain readable. The check shares the caller's transaction and
snapshot and runs before UPDATE or DELETE side effects.

## Validation checkpoints

The following are completed local checkpoints, not competition scores:

| Tested source | Evidence |
| --- | --- |
| `7d8005cefc2901573395eb58fd4f55ab210d51e4` | Release build; 36 independent DDL checks; restart persistence; four existing AI function suites |
| `9b1f59e2c07ce5c1f323a17df1163dd2c6033e36` | Release build; eight worker/DML groups including rollback, typed values, grouping, stale responses and batching |
| `f4de37c73c9822804bdde96679008f363166b237` | Release build; six native RETRY groups including pseudo-status, quoted status and transaction rollback |

The five official AI Column tests are located in
`tools/deploy/mysql_test/test_suite/ai_column/`, rather than the `ai_function/`
directory mentioned in the task description. The initial run against the
pre-catalog checkpoint did not pass. Official test and result files remain
unchanged. Catalog and strict-read changes require a new successful build and
runtime results before this checkpoint table can be extended.

Independent fault fixtures and reports live outside the repository under the
local build laboratory. They are never loaded into the product. Their inputs
and model responses are not encoded in `src/`.

## Remaining validation

Run all five unmodified AI Column suites; repeat affected endpoint/AI function
regressions; verify strict mode with prepared plans, views and uncommitted
invalidation; verify task grouping and quoted composite keys; exercise vector
index invalidation and recovery. Crash recovery, large-table queue scanning,
non-administrator catalog permissions and complex ALTER/view-write cases need
separate evidence before treating this implementation as production ready.
