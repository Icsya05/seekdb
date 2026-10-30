"""Actual production parser; JSON/allocation boundaries use test doubles."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


class EmbeddingResponseTypes(unittest.TestCase):
    def test_provider_checks_response(self):
        text = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
        start = text.find('// Embedding response validation.')
        if start < 0:
            start = text.index('int ObOpenAIUtils::ObOpenAIEmbed::parse_output(')
        end = text.index('int ObOllamaUtils::get_header', start)
        scaffold = pathlib.Path(__file__).with_name('embed_response_harness.cpp').read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'parser.cpp'
            exe = pathlib.Path(directory) / 'parser'
            cpp.write_text(scaffold.replace('/* ACTUAL_PARSER */', text[start:end]))
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
            for case in ('containers', 'order', 'duplicate', 'mixed', 'negative',
                         'range', 'fractional', 'string', 'empty', 'infinity'):
                with self.subTest(case=case):
                    subprocess.run([str(exe), case], check=True)
