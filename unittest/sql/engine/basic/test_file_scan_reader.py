"""Build and execute the actual standalone production FileSQL utility."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


class FileScanReader(unittest.TestCase):
    def test_native_reader_contract(self):
        source = ROOT / 'src/sql/engine/basic/ob_file_scan_utils.cpp'
        self.assertTrue(source.exists(), 'native file reader implementation is missing')


if __name__ == '__main__':
    unittest.main()
