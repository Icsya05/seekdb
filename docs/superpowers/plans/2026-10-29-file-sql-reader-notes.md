# FileSQL reader utility implementation notes

Scope: `src/sql/engine/basic/ob_file_scan_utils.{h,cpp}` and standalone reader
tests under `unittest/sql/engine/basic`. Parser, system variables, SQL binding,
physical scan operators and actual SQL validation belong to the integration
work stream. This utility is not the complete FileSQL task.

## Contract

- CSV and flat JSONL, bounded to 8 MiB per logical record, 1,024 columns,
  1 MiB of schema names/column metadata and 2 MiB serialized binding.
- Inference scans every record, retains only schema and the current record,
  and accepts `sample_size=-1` only. The scanner has a 64 KiB read buffer.
- `infer_file(..., error, check_status)` and `Scanner::next(..., error,
  check_status)` accept an optional `std::function<int()>`. Nonzero native
  status is returned unchanged; checks occur per record and read-buffer refill.
  Callbacks are transient and never retained in the serialized binding.
- `Cell.text` is decoded CSV / scalar JSON text. NULL is identified only by
  `Cell.is_null`; a quoted empty CSV value is a non-NULL empty string.
- BIGINT is signed 64-bit. Finite decimal/exponent values widen integers to
  DOUBLE. BOOLEAN accepts case-insensitive true/false in CSV. DATE and
  DATETIME validate the Gregorian calendar and time fields; date+datetime
  widens to DATETIME. Incompatible types widen to VARCHAR. All-NULL columns
  retain `NULL_VALUE` and are nullable. `max_length` is decoded byte length,
  including date/datetime text and fractional seconds when present.
- CSV names are preserved; empty headers become column1, column2, etc.
  Repeated ASCII-case-insensitive names get deterministic __2, __3 suffixes.
- JSONL schema order follows the first appearance of each key, missing keys
  become NULL, and empty objects participate in the field union. Nested values,
  duplicate keys, case-only conflicting keys, malformed UTF-8 and nonfinite
  numbers are rejected. Strings retain their JSON string type, except valid
  date/datetime strings; actual numbers and booleans retain their scalar types.

## File boundary

Linux opens walk directory descriptors with `openat` and `O_NOFOLLOW`; all
symlinks are rejected, including symlinks in the policy directory. Empty/NUL
policy, parent traversal and paths outside the policy directory fail closed.
Relative paths are anchored at `secure_file_priv`, and bound paths are absolute.
FIFO/device/directory inputs are rejected after nonblocking open and fstat.
Other platforms return UNSUPPORTED rather than use a less safe open path.

An open scanner pins its FD. File identity, size, mtime and ctime are compared
against the inferred fingerprint; the bound path is safely reopened to detect
replacement. Checks run during reads, at EOF, on returned scan records and
before rescan. Inference validates the final fingerprint after its full pass.
The actual read byte count must also match the size fingerprint; pseudo-files
that report zero size while yielding data are rejected. Timestamp comparison
tokens use defined unsigned arithmetic, including filesystem dates beyond 2262.
Following any scan error, callers must fail the query; rescan explicitly resets
record state. Closing/reopening never reuses an old descriptor.

## Verification ledger

- Initial real-file harness: 20 missing-implementation failures before C++ code.
- First implementation: 20/20 UBSan cases passed, including a roughly 135 MB
  file inferred under a 96 MiB address-space limit.
- Additional RED cases exposed a valid UTF-8 EF prefix being mistaken for a
  BOM, an empty-object union rejection, and a million-level JSON nesting crash.
  The parser now checks exact BOM bytes, permits empty union rows, and rejects
  nested JSONL / excessive binding depth before RapidJSON recursion.
- Cancellation RED observed missing callback API and then incorrect conversion
  of the native cancellation code; the final interface propagates native codes.
- The /proc/version size mismatch was reproduced RED and rejected after adding
  actual-byte accounting. A valid year-2400 filesystem timestamp reproduced a
  UBSan signed-overflow failure; timestamp tokens now avoid signed overflow.
- Final production-function suite: 28/28 passed under UBSan, including native
  cancellation status propagation, long-record cancellation and rescan.
- Final production translation unit compiled to an object with the actual native
  compiler and build flags, exit 0. Full engine link and real SQL checks are
  coordinated separately by the integration work stream.

Run the standalone suite with `FILE_SQL_DEPS_INCLUDE` pointing at the project's
bundled include directory containing `rapidjson/document.h`, then
`python3 -m unittest discover -s unittest/sql/engine/basic -p test_file_scan_reader.py -v`.

## Remaining boundaries

Parquet, glob, DESCRIBE and COPY are separate features. This utility does not
provide them, and does not execute SQL or use an external query engine.
