# Endpoint concurrency follow-up — 2026-11-01

The `max_concurrency` failure recorded in `AI_FUNCTIONS_VALIDATION.md` is fixed in this follow-up. All four checked-in official AI mysqltests now pass locally. No official competition evaluation was submitted and no competition score is claimed.

## Implementation

Endpoint JSON accepts an integer `max_concurrency` from 1 through 1024, defaulting to 16. CREATE, ALTER, copying of endpoint metadata, the persistent system table and `DBA_OB_AI_MODEL_ENDPOINTS` retain this value. Invalid values fail with the expected parameter-value error; unrelated ALTER operations preserve the existing limit.

SQL AI model requests acquire a shared process-local permit before each HTTP attempt. Endpoints with identical URL strings use the minimum configured limit for that URL. Separate endpoints cannot obtain independent allowances for that same URL. Each attempt releases its permit on completion, errors or timeout; retry backoff does not hold a permit. Waiting respects the query deadline. Existing in-flight calls are allowed to finish. Limits are resolved when a query reads endpoint metadata; the tests change limits between request waves, not during an in-flight metadata change.

The limited multi-request path preserves request/result order and acquires one permit per request. Cross-row embedding still sends multiple input strings in one HTTP request. Response bodies are reset between retries, so a partial response from a disconnected attempt cannot be concatenated with the next response. HTTP timeouts account for time spent waiting, and response buffers are bounded at 16 MiB. Endpoint parameter logging and HTTP-header logging no longer print credential values.

## Exact build and results

- Previous AI delivery: `b84c3aa22efa727977ef03605c913c30238f9d52`.
- Tested source: `0326ecc704f01fc3a31d5c815b5ed96196baa337`.
- Binary SHA-256: `63798b32c5fec846d70a4ea5b289e4f71cd5f02f477f3b68f07e11893233c249`.
- Same Linux Release build environment as the first AI delivery: LLVM 17, Rust 1.98.1, 6-CPU/10-GiB build container, `make -j4`; database CPU count 2 and memory budget 2 GiB.
- Validation uses a separate fresh database directory, then explicitly stops and restarts that database to verify persistence. Earlier baseline, AI candidate and File SQL database directories and binaries are preserved.

| Check | Result |
| --- | --- |
| Official `ai_prompt` | passed |
| Official `ai_function_enhancement` | passed |
| Official `ai_model_endpoint_ddl` | passed, including the previously blocked CREATE and later ALTER/privilege checks |
| Official `ai_model_ddl` | passed |
| Existing independent AI function groups | 12/12 passed |
| New endpoint concurrency/fault groups | 5/5 passed |
| Restart and persisted-limit check | passed |

The new checks execute real concurrent SQL sessions against a local HTTP service:

- Eight requests with limit 1 produced HTTP peak concurrency 1. After ALTER to 2, eight requests produced peak 2. Every response matched its input.
- Two endpoints configured with limits 2 and 5 shared one URL; eight mixed requests produced peak 2.
- A 404 request failed and subsequent requests succeeded. A one-time 429 and a deliberately truncated first HTTP response each caused exactly two upstream requests and returned the correct final result.
- While a limit-1 endpoint was occupied, a second query with a 100 ms deadline failed after 0.102 seconds. Its prompt never reached the HTTP service. The active request and a later request both completed successfully.
- After restart, configured limits 2, 2 and 5 remained visible in the system view, and a normal completion call succeeded.

The official test scripts, result files and data were not edited. All product changes are in `src/`; this new document records the follow-up. Test fixtures and scripts are separate delivery artifacts. Tests use synthetic local credentials and no real model service.

## Reproduction and scope

The evidence archive includes `build-endpoint.sh`, `validate-endpoint.sh`, the existing AI mock/regression scripts, the new concurrency fixture, restart check and raw reports. In the dedicated build container:

```sh
bash /lab/build-endpoint.sh 0326ecc704f01fc3a31d5c815b5ed96196baa337
# Start the supplied mock-ai.py on container port 8000 if it is not already running.
bash /lab/validate-endpoint.sh
```

The concurrency fixture binds container loopback port 8001 for its lifetime. The final summary is `official=0 runtime=0 endpoint=0 restart=0`. The validation runner stops its database afterwards.

The limit is shared inside this single seekdb process and keyed by an identical URL string. Different textual URL aliases or multiple seekdb processes are not consolidated. This validates SQL AI function requests and the tested endpoint metadata path; it does not implement asynchronous AI columns. The earlier document remains a record of the initial build rather than being rewritten to claim tests that had not passed at that time.
