# FileSQL CSV/JSONL integration checkpoint

This implements the first native integration slice from
`2026-10-29-file-sql.md`; it is not the completed 50-point FileSQL task.

## Implemented path

`read_csv`, `read_json` and `read_ndjson` in FROM bind literal paths and named
options, infer the complete schema using bounded reader buffers, and expose
typed native column expressions. FUNCTION_TABLE logical paths select a dedicated
vectorized PHY_FILE_SCAN. The scan preserves source column ordinals after
projection and permits COUNT(*) with no referenced output columns. SQL joins,
filtering, aggregation, sorting and LIMIT use ordinary seekdb operators.

File SQL defaults OFF through generated enable_file_sql metadata. Binding and
execution read current global secure_file_priv; the utility enforces strict
directory containment without the existing export allowlist. Execution/rescan
rechecks authorization and file identity. Native cancellation errors propagate
from full inference and long-record reads. Bound path/options are excluded from
automatic SQL parameterization, and file queries disable plan caching and
parallel execution.

## Evidence and remaining gate

- Reader implementation: 28 real-file UBSan cases passed in its independent
  implementation workstream, including bounded-memory full inference, path
  restrictions, changed files, rescans and cancellation. Its production object
  compiled successfully before integration.
- Native SQL baseline on the unchanged 140f binary: 14 tests ran; 13 failed on
  missing enable_file_sql, while ordinary SELECT passed. The current suite has
  16 cases. New-binary SQL results are still pending.
- Bison 2.4.1 generation completed with no conflicts after fixing optional-alias
  precedence and a redundant ESCAPE production.
- Actual production flags produced all 11 changed C++ implementation objects:
  FileScanOp, the dedicated file binder, static codegen, operator factory,
  resolver utilities, DML resolver, DML statement, basic session, system-variable
  initialization, metadata and factory. Factory standalone compilation first
  exposed missing OV/OX macro declarations; an explicit ob_check_macros.h include
  was added to the generator and output, then its object compiled successfully.
  Full native build and SQL execution remain a separate gate.
- SQL inventory ownership, Python test syntax and git whitespace checks passed.

Run native tests only on the selected isolated instance:

```
SEEKDB_FILE_SQL_TEST=1 SEEKDB_PORT=12881 python \
  unittest/sql/engine/basic/test_file_sql_native.py
```

The test class saves global secure_file_priv, temporarily sets it to its fixture
root (default /tmp; override SEEKDB_FILE_SQL_FIXTURE_DIR), and restores it during
class cleanup. It creates no database tables or imported data.

Parquet, glob, DESCRIBE schema output and COPY are not implemented by this slice.
No supplied official tests, benchmark data or third-party dependencies changed.

## Generation notes

enable_file_sql uses new system-variable ID 10754. Parser nodes 4921/4922 and
PHY_FILE_SCAN are appended without renumbering existing entries. Generated
system-variable files derive from ob_system_variable_init.json. The existing
Python 2 generator was executed with an in-memory iteritems-to-items compatibility
substitution; equivalent existing initializer blocks were retained to avoid
Python 2/3 ordering and Unicode-repr churn. Temporary scripts, objects and logs
remain under .superpowers and are excluded from the implementation commit.
