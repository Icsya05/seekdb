# Ollama provider implementation and verification ledger

**Goal:** Support case-insensitive Ollama endpoint metadata, optional access keys,
native non-streaming completion and native embedding, as required by the official
2026 preliminary AI-function specification.

**Scope:** Reuse the existing Ollama implementations. Change only endpoint
validation, provider dispatch and Ollama-specific request/response methods.
OpenAI response ordering and cross-row embedding validation belong to the parent
integration task. No model names, inputs or expected outputs are hardcoded in
production code.

- [x] Reproduce keyless endpoint creation failure through actual DBMS_AI_SERVICE
  SQL on the installed released server: error 11114, empty access_key.
- [x] Add production-method component tests in the established expression unittest
  directory; observe metadata, dispatch, stream, header and response-type failures.
- [x] Allow OLLAMA in the provider whitelist and exempt only this provider from the
  existing nonempty-key requirement. Existing case-insensitive comparison remains.
- [x] Select existing native Ollama completion and embedding implementations.
- [x] Send JSON content type without authorization; force stream:false after
  merging completion config, since the SQL client consumes one JSON response.
- [x] Reject empty/non-string completion and non-array embedding envelopes before
  downstream callers access their contents.
- [x] Run component regressions with UBSan: 7 passed (6 failed before the fix;
  native embedding body was an existing passing control).
- [x] Compile both changed production cpp files to real objects using pinned
  Clang 17, production CMake flags and the actual generated Rust bridge headers:
  both exit 0. Commands and logs are saved in the workspace's research build log.
- [ ] Run opt-in real SQL regressions against a newly built server. The installed
  released server is used only to demonstrate pre-fix failures.

The component harness executes extracted, unchanged production method bodies
with JSON/allocation boundary doubles; it does not prove real JSON implementation
behavior or SQL integration. The production object compile checks real headers
and APIs; it does not prove a full link or full-server runtime behavior.

**Review focus:** changing an OpenAI endpoint to keyless Ollama; switching a
keyless endpoint back to a key-required provider; omitted versus empty keys;
stream:true supplied in config; malformed response envelope types. Component
tests and opt-in SQL tests cover these at the applicable boundaries.
