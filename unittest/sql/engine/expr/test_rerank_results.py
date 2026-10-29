"""Execute production rerank result handling with JSON/allocation doubles.

The harness compiles the actual function definitions, not a Python port.
Run with Python unittest discovery; g++ and UBSan are required.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


class RerankResults(unittest.TestCase):
    def test_result_mapping_and_stable_merge(self):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp').read_text()
        functions = []
        for name in ('get_doc_array_from_documents_array_with_key',
                     'sort_document_array_by_model_result',
                     'batch_result_add_base', 'compact_json_array_by_key'):
            start = source.index('int ObExprAIRerank::' + name + '(')
            end = source.index('\nint ObExprAIRerank::', start + 1)
            functions.append(source[start:end])
        provider_source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp').read_text()
        for provider in ('ObDashscopeUtils::ObDashscopeRerank',
                         'ObSiliconflowUtils::ObSiliconflowRerank'):
            start = provider_source.index('int ' + provider + '::parse_output(')
            end = provider_source.index('\nint ', start + 1)
            functions.append(provider_source[start:end])
        scaffold = pathlib.Path(__file__).with_name('rerank_results_harness.cpp').read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = pathlib.Path(directory) / 'rerank.cpp', pathlib.Path(directory) / 'rerank'
            cpp.write_text(scaffold.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(functions)))
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(cpp), '-o', str(exe)], check=True)
            for case in ('unequal', 'ties', 'unsorted', 'integer_score', 'range',
                         'negative', 'duplicate', 'fractional_index', 'string_score',
                         'infinity', 'object_type', 'offset', 'object_integer_score',
                         'object_range', 'doc_key', 'missing_doc_key',
                         'siliconflow_valid', 'siliconflow_container',
                         'dashscope_valid', 'dashscope_output', 'dashscope_container'):
                with self.subTest(case=case):
                    subprocess.run([str(exe), case], check=True)


if __name__ == '__main__':
    unittest.main()
