"""Build and execute the actual standalone production FileSQL utility."""
import pathlib
import subprocess
import tempfile
import unittest
import os

ROOT = pathlib.Path(__file__).resolve().parents[4]


class FileScanReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = ROOT / 'src/sql/engine/basic/ob_file_scan_utils.cpp'
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = pathlib.Path(cls.temp.name) / 'reader'
        if cls.source.exists():
            candidates = [os.environ.get('FILE_SQL_DEPS_INCLUDE', ''), '/usr/include',
                          str(ROOT / 'deps/3rd/usr/local/oceanbase/deps/devel/include')]
            include = next((p for p in candidates if p and (pathlib.Path(p) / 'rapidjson/document.h').exists()), None)
            if include is None:
                raise unittest.SkipTest('set FILE_SQL_DEPS_INCLUDE to the bundled RapidJSON include directory')
            subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I' + str(ROOT / 'src'), '-I' + include,
                            str(cls.source), str(pathlib.Path(__file__).with_name('file_scan_reader_harness.cpp')),
                            '-o', str(cls.exe)], check=True)

    def run_case(self, name):
        self.assertTrue(self.source.exists(), 'native file reader implementation is missing')
        subprocess.run([str(self.exe), name], check=True, timeout=60)

    def test_csv_quote_utf8_crlf_multiline_and_null(self): self.run_case('csv')
    def test_csv_escape_and_options(self): self.run_case('options')
    def test_duplicate_headers_and_empty_names(self): self.run_case('headers')
    def test_no_header_and_type_widening(self): self.run_case('types')
    def test_real_calendar_dates_and_numeric_boundaries(self): self.run_case('calendar')
    def test_inference_visits_last_row(self): self.run_case('full_schema')
    def test_empty_and_header_only(self): self.run_case('empty')
    def test_malformed_records_and_encoding(self): self.run_case('invalid')
    def test_record_column_and_metadata_limits(self): self.run_case('limits')
    def test_fail_closed_paths_and_nonregular_files(self): self.run_case('paths')
    def test_symlink_and_parent_directory_escape(self): self.run_case('symlinks')
    def test_secure_directory_swap_cannot_escape(self): self.run_case('directory_swap')
    def test_file_mutations_detected_before_open_and_during_scan(self): self.run_case('changed')
    def test_path_replacement_detected(self): self.run_case('replaced')
    def test_rescan_and_close_reopen(self): self.run_case('rescan')
    def test_binding_roundtrip_and_untrusted_metadata(self): self.run_case('binding')
    def test_jsonl_union_scalars_nulls_and_types(self): self.run_case('jsonl')
    def test_jsonl_rejects_nested_duplicate_and_invalid_records(self): self.run_case('json_invalid')
    def test_jsonl_late_keys_and_type_conflicts(self): self.run_case('json_union')
    def test_large_file_inference_under_memory_limit(self): self.run_case('bounded')
    def test_utf8_header_starting_with_ef_is_not_assumed_bom(self): self.run_case('utf8_prefix')
    def test_deep_json_input_is_rejected_without_recursion(self): self.run_case('depth')
    def test_records_cross_internal_read_chunks(self): self.run_case('chunks')
    def test_empty_json_objects_participate_in_union(self): self.run_case('json_empty')
    def test_full_inference_honors_query_cancellation(self): self.run_case('cancel')
    def test_scan_interrupts_inside_long_record_and_can_rescan(self): self.run_case('scan_cancel')
    def test_dynamic_proc_regular_file_cannot_pass_size_fingerprint(self): self.run_case('proc_size')
    def test_far_future_filesystem_timestamp_does_not_overflow(self): self.run_case('future_time')


if __name__ == '__main__':
    unittest.main()
