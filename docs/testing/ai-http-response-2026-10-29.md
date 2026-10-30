# AI HTTP response validation

The shared client previously downcast parsed arrays, strings and JSON null to
`ObJsonObject` without checking type. Actual production-method boundary tests
reproduced invalid downcasts under UBSan. Batch collection also ignored failed
curl transfers/getinfo and rejected successful HTTP 201 despite the scalar path
accepting 2xx. Failures could retain a caller's old response pointer.

The response paths now clear the scalar output, require an object root, preserve
transport failures and apply the same 2xx status rule. Error reporting also
rejects a null JSON node before printing it; this prevents malformed-root
errors from reaching a preexisting null dereference in completion/embedding
error formatting.

Actual production methods with curl/parser/allocation boundary doubles:
17 transport/root/status scenarios plus one null-printer scenario, 14 baseline
failures including UBSan errors, then all scenarios passed after correction.
These doubles verify control flow, not the real curl/parser integration.
Both modified production translation units compiled with native Clang 17 flags
(exit 0). The full binary passed real SQL malformed-root, recovery and HTTP 201
cases. See [integration verification](ai-function-integration-2026-10-29.md).
