# AI function integration verification

2026-10-30 update: production and SQL tests at
`8f2f75122f3c009427c7588ee7f3bc4b42c10ee6` passed a full build and 51 real SQL
cases (three additional JSON_LENGTH cases). Both batch-wrapper regressions are
GREEN; the unchanged official enhancement script exited 0 and its complete
recorded output matched the earlier 140f output byte for byte.
The historical 48-test table below describes the earlier 140f run.

Production commit: `140f2f14d7d44fc3869ed933285a88b0121668be`.
SQL test commit: `3f4dc4ffc7f8449bd460f89ed97637fa3d1bd909` (test-only delta).

## Behavior

- AI_PROMPT accepts JSON and TEXT/LONGTEXT, repeated and nested placeholders,
  Unicode and escapes; rejects invalid indices and capacity narrowing.
- AI_EMBED propagates NULL content and validates positive dimensions and finite,
  nonempty numeric vectors. Indexed OpenAI responses are restored to input order.
- AI_COMPLETE handles omitted/NULL/native JSON/text configuration and rejects
  empty/nonstring content and malformed HTTP envelopes without losing SQL connectivity.
- Ollama provider matching is case insensitive; missing API keys work, including
  repeated ALTER; completion and embedding use native nonstreaming protocols.
- AI_RERANK shares validated 20-item batches across string/object inputs, restores
  global indices, retains object IDs and stably sorts descending scores.
- Direct AI_EMBED scans group compatible model/dimension rows into one request
  per engine batch; NULL, filtering, LIMIT and multiple batches have SQL coverage.

## Executed checks

Full native production CMake target `seekdb` built and linked successfully using
the pinned Clang 17 toolchain, real Rust Release libraries and generated bridge
headers. The executable's version identifies the exact production commit.
No production header or library was replaced with a test double.

On an isolated local server at port 12881, with HTTP mock services only:

| Suite | Passed |
| --- | ---: |
| test_ai_prompt_enhancement.py | 8 |
| test_complete_sql.py | 9 |
| test_embed_batch_sql.py | 13 |
| test_embed_null_sql.py | 6 |
| test_ollama_provider.py | 7 |
| test_rerank_sql.py | 5 |
| Total | 48 |

No failures, errors or skipped SQL tests. Completion tests include array, string,
JSON null and invalid JSON roots followed by SELECT 1 and valid recovery;
HTTP 201 is accepted consistently with other 2xx responses.

The unchanged official `ai_function_enhancement.test` executed through the real
mysqltest client with exit 0. Its recorded output and local HTTP requests passed
13 additional functional assertions (completion/config, prompt types, dimensions,
NULL mapping, rerank count/original ID and Ollama). No official expected result
file was supplied; this is not a golden-file comparison.

Reproduce with PyMySQL, an isolated new binary and the switches documented in
the individual suites, for example:

```sh
SEEKDB_PORT=12881 SEEKDB_PROMPT_TEST=1 SEEKDB_COMPLETION_TEST=1 \
SEEKDB_EMBED_BATCH_TEST=1 SEEKDB_EMBED_NULL_TEST=1 \
SEEKDB_OLLAMA_TEST=1 SEEKDB_RERANK_TEST=1 \
python3 -m unittest discover -s unittest/sql/engine/expr -p 'test_*.py'
```

Component tests also run under this command. Models, endpoints and tables use
random test-only names and are cleaned up. No real model credentials are needed.

## Limits

This is local verification, not a competition score or exhaustive regression run.
The earlier JSON_LENGTH scalar wrapper bypassed child batching. This was fixed
in the 2026-10-30 update: an eight-row wrapped projection makes one eight-input
request; path/NULL/filter/LIMIT and ordinary JSON semantics also pass. Other
scalar wrappers have not all been adapted, so batching through arbitrary
wrappers is not claimed.

Dashscope ordering and external-provider maximum batch sizes were not verified
against real services. SQL model requests use deterministic local HTTP mocks.
FileSQL and asynchronous AI_COLUMN remain separate tasks.
