# Native File SQL validation — 2026-11-01

File SQL now runs through seekdb's parser, resolver, optimizer and execution operators. It reads local CSV, JSONL and flat Parquet files without creating an imported table or delegating SQL to another engine. `COPY` writes CSV through a temporary file and publishes only after query success and input validation.

This is local validation, not an official competition score. No judge submission was made.

## Revisions and build

- Feature branch: `feat/competition-file-sql`.
- Shared merge baseline: `ed5968fe812281686ce3498a6b2226bb57cd5d8a`.
- Official competition test revision: `5bce9e06d1281347ad61344e4b01fab9158c58a2`.
- Tested source: `f89009c02f5ba3fa12ca29151e53a047ca3d14e3`.
- Release binary SHA-256: `5bb5e3f7c1454847bea270a3570869405536bccb3c7fbcd3f0d921b7dead1575`.
- Build: Linux x86-64, CentOS Stream 9, LLVM 17, Rust 1.98.1, `bash build.sh release`, `make -C build_release -j4 seekdb`.
- Dedicated Docker container: 6 CPU limit, 10 GiB memory; tested seekdb instance: 2 CPU, 2 GiB memory budget. Its source tree had no tracked changes relative to the tested commit.

The parent team's history is preserved. Implementation changes are confined to `src/`; this document is new. Official test files, result files and data were not edited. AI function changes from the separate feature branch are not included here.

## Results

| Validation | Result |
| --- | --- |
| Independent basic SQL groups | 21/21 passed |
| Independent Parquet, projection, COPY and execution groups | 23/23 passed |
| Independent correctness, security and lifetime groups | 15/15 passed |
| Official `file_sql/file_sql_basic` | passed |
| Official FTS `tokenize_function`, `simple_query`, `parser_properties` | 3/3 passed |
| Official `ai_function/ai_model_ddl` | passed |

The independent tests execute SQL against the actual Release server. PyArrow 21.0.0 generates Parquet fixtures and verifies the deliberately damaged payload independently; it is a test dependency, not a product dependency or query executor. Test coverage includes:

- CSV quotes, escapes, embedded newlines, CRLF, UTF-8, duplicate headers, missing values, bad records, integer boundaries, dates and timestamps.
- JSONL field union, NULL values, missing fields, widening and rejection of nested or ambiguous fields. All-NULL columns describe as `NULL`.
- Parquet V1/V2 pages, uncompressed/Snappy, plain/dictionary/boolean RLE encodings, required and optional columns, all-NULL columns, multiple row groups and data pages, empty files and checksum errors.
- Projection reads only referenced Parquet columns: an unselected gzip column does not prevent selecting the supported column. `COUNT(*)` can use the footer row count without decoding column pages.
- Native expressions, filters, aggregation, sorting, limits, inner/left/cross-format joins, nested-loop rescans and row/batch settings.
- COPY from a table or query, text/large-text values, delimiters, headers, NULL versus empty strings, readback, no overwrite and cleanup after partial failure.
- Default-off access, empty/NULL secure root, path-prefix attacks, symlink escapes, record/column limits, input changes during COPY including `LIMIT 1`, and file descriptor release after 60 successful/failed-query cycles.
- Ordinary SQL, transactions, JSON_TABLE and ordinary `DESC SELECT` retain their existing behavior.

One generated CSV contained 120,000 rows and 246,608,901 bytes. The final build produced the exact expected count and sums in 3.273 seconds. Whole-process RSS rose from 222,367,744 to at most 224,477,184 bytes during that query (2,109,440 bytes growth). This is one local measurement, not a general performance guarantee. A 10,000-microsecond timeout stopped full-schema inference in 0.020 seconds with the database timeout error. Descriptor checks found zero input-file descriptors before and after the repeated-query test.

## Execution and safety

File tables use `ObLogFunctionTable` / `ObFunctionTableOp`, with file-specific metadata and projected column indices. CSV/JSONL infer schema from the complete stream, then open a new read-only descriptor for execution. Parquet uses its footer schema. File-query plan caching is disabled so later SQL executions re-infer changed files.

Path checks canonicalize `secure_file_priv` and the target, enforce the directory boundary, verify opened descriptors, and reject non-regular input files. Device, inode, size, mtime and ctime identify the input snapshot; changes are checked during scans, rescans and before COPY publication. COPY creates an exclusive temporary file, publishes the opened inode without replacing an existing target, and removes its owned temporary file on failure.

Resource limits include 16 MiB per CSV/JSONL record or Parquet page, 1,024 columns, 256-byte column names, bounded Parquet metadata and a 128 MiB page/dictionary budget. `glob` bounds both match count (100,000) and accumulated path bytes (16 MiB) while enumerating the directory. File scans use bounded batches rather than whole-file materialization.

## Reproducing the recorded run

The accompanying evidence archive contains the independent scripts, build log, raw results, official mysqltest execution logs, source patch and hashes. Mount its lab files at `/lab` and a checkout containing the feature commit at `/source` in the dedicated Linux build environment; `/workspace` is the writable checkout/build directory. Install the normal seekdb build dependencies, PyMySQL under `/lab/python-deps`, and PyArrow 21.0.0 under `/lab/parquet-test-deps`.

```sh
bash /lab/build-file-sql.sh f89009c02f5ba3fa12ca29151e53a047ca3d14e3
PYTHONPATH=/lab/python-deps python /lab/setup-file-test-ssh.py
bash /lab/validate-file-sql.sh
```

The official File SQL include script deploys fixtures through real SSH/SCP. The setup helper creates an isolated, key-only SSH service inside the test container, pins its host key, and does not expose that port on the host. Its private keys are excluded from the evidence archive. Fixture deployment and cleanup are restricted to `/workspace/.local/official-file-sql`. The test SQL and expected result remain the checked-in official files.

The validation runner starts and stops its own server. Scripts write separate JSON/log reports for basic, advanced, integrity, official File SQL, FTS and AI DDL checks. The final shell summary is `basic=0 advanced=0 integrity=0 official=0 fts=0 ai=0`.

## Scope and remaining limits

- This is the single-process local-file path. It does not implement remote files, automatic multi-file unions or distributed file scans.
- CSV/JSONL support only `sample_size=-1`; JSON input is newline-delimited flat objects. Glob wildcards apply to the basename within one directory.
- Supported Parquet compression is uncompressed and Snappy. Supported data encodings are plain, dictionary and boolean RLE. Nested/repeated schemas, INT96, decimals, unsigned logical integer types and other codecs/encodings are not implemented; selected unsupported data returns an error. Timestamp precision beyond exact microseconds is rejected.
- The validated target is Linux. COPY publication deliberately returns unsupported on other platforms.
- The parser/file-scan unit-test paths mentioned in the task description are absent from the official revision used here; they were not run. The checked-in official mysqltest and the independent SQL tests above were run.

Format references: [Parquet encoding specification](https://parquet.apache.org/docs/file-format/data-pages/encodings/), [Parquet Thrift schema](https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift), [Thrift compact protocol](https://github.com/apache/thrift/blob/master/doc/specs/thrift-compact-protocol.md), and [Snappy format](https://github.com/google/snappy/blob/main/format_description.txt).
