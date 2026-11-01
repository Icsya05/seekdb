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

## Completed local validation

Tested Release source: `c2cd9f4282b380848d03897ddd4b0949c4ae7c91`.
Binary SHA-256: `6a3a7a6a8e7335b0b3f93eea7a37e519b437d24e60f8baf7c2db6f69e3eaf917`.
The subsequent delivery commit only updates this document; its `src/` tree is
identical to the tested source. This is local validation, not an official score.

All five unmodified official AI Column mysqltests passed, including their exact
expected-result comparisons: `ai_column_ddl`, `ai_column_t1_dml`,
`ai_column_worker`, `ai_column_retry_strict`, and `ai_column_vector_index`.
Their actual directory is `tools/deploy/mysql_test/test_suite/ai_column/`.
Official tests, result files and benchmark files are byte-for-byte unchanged
from upstream competition revision `5bce9e06d1281347ad61344e4b01fab9158c58a2`.

| Independent checks on the tested binary | Passed |
| --- | ---: |
| DDL, definitions, invalid expressions and ordinary SQL | 36 |
| Worker, typed/grouped results, DML, stale responses and batch claims | 8 |
| Conditional RETRY and rollback | 6 |
| Strict reads, prepared-plan toggling, views and caller transactions | 7 |
| Task grouping, quoted composite keys and catalog plan invalidation | 3 |
| Actual HTTP retry budgets | 2 |
| Views, stored routines, column removal/recreation and input DDL guards | 4 |
| Offline column-ID remapping | 1 |
| Crash recovery | 1 |
| Definition persistence after restart | 1 |
| Total | 69 |

With `max_retries=0`, persistent HTTP 503 caused exactly one upstream request.
With `max_retries=2`, two HTTP 503 responses followed by success caused exactly
three requests. A guarded test killed only the task-owned database process
during an HTTP request. READY data survived restart; committed PENDING work
recovered after lease expiry in 64.296 seconds, with exactly two actual requests.

The column-ID test drops an earlier column and changes the input type through
an offline rebuild. Dependencies and hidden state names are remapped, and the
native DDL copy/checksum mapping carries the implicit state renames. A later
input update produces the new result. Dropping an AI result also drops its
hidden state in the same DDL transaction; adding it again after a rebuild works.

Four existing AI function suites (`ai_prompt`, `ai_function_enhancement`,
`ai_model_endpoint_ddl`, `ai_model_ddl`), official `file_sql_basic`, and three
FTS suites (`tokenize_function`, `simple_query`, `parser_properties`) also passed.

Task claims do not assign NULL to already-PENDING results: business DML has
already invalidated them. This avoids redundant vector index versions and
preserves the official vector plan/result comparison without modifying tests
or hardcoding costs. A redefined terminal result is invalidated when claimed.

Reproduction commands inside the prepared local build laboratory:

```sh
bash /lab/build-ai-column.sh c2cd9f4282b380848d03897ddd4b0949c4ae7c91
PYTHONPATH=/lab/python-deps python3 /lab/server-control.py start ai-column
python3 /lab/check-official.py --label ai-column-21 --suite ai_column --tests \
  ai_column_ddl ai_column_t1_dml ai_column_worker ai_column_retry_strict ai_column_vector_index
python3 /lab/run-ai-column-independent.py 21 ddl worker retry strict catalog budget lifecycle remap recovery
PYTHONPATH=/lab/python-deps python3 /lab/check-ai-column-ddl.py --restart
```

Independent scripts, local mock fixtures, complete reports and build logs are
packaged outside the repository. Historical failed checkpoints remain distinct
from the successful `21` reports. No test input, model name, key or response is
encoded into product behavior. No official judge submission was made.

## Limits and follow-up work

Renaming or dropping an input referenced by a live AI definition is rejected
atomically. Drop the AI definition first if that input needs to be renamed or
removed. Updating AI inputs through a view is rejected; direct result assignment
is rejected through both views and stored routines. Ordinary unrelated view
updates and stored-routine input updates retain native semantics.

Validation used a local single-process instance, mock HTTP services and
administrator accounts. Large-table queue scanning, non-administrator catalog
permissions, broad online expression redefinition under concurrent readers,
and throughput at competition scale need separate evidence before production
deployment. The four polling workers and full catalog expansion are correctness
implementations, not claims of optimal throughput.
