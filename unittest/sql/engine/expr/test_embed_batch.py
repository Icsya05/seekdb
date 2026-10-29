"""Execute the production batch evaluator with engine/transport boundaries.

The SQL companion test exercises the real engine and HTTP provider end to end.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[4]


class EmbedBatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp').read_text()
        start = source.find('int ObExprAIEmbed::eval_ai_embed_batch(')
        cls.actual = None if start < 0 else source[start:source.index('int ObExprAIEmbed::eval_ai_embed(', start)]
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable = pathlib.Path(cls.temp.name) / 'batch'
        if cls.actual:
            scaffold = pathlib.Path(__file__).with_name('embed_batch_harness.cpp').read_text()
            cpp = cls.executable.with_suffix('.cpp')
            cpp.write_text(scaffold.replace('/* ACTUAL_BATCH */', cls.actual))
            subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(cpp), '-o', str(cls.executable)], check=True)

    def run_case(self, name):
        self.assertIsNotNone(self.actual, 'AI_EMBED has no native batch evaluator')
        subprocess.run([str(self.executable), name], check=True)

    def test_multiple_inputs_one_request(self): self.run_case('batch')
    def test_null_rows_omitted_without_result_shift(self): self.run_case('null')
    def test_all_null_no_requests(self): self.run_case('all_null')
    def test_skip_and_evaluated_rows_preserved(self): self.run_case('skip')
    def test_scalar_content_argument(self): self.run_case('scalar_content')
    def test_model_and_dimension_groups(self): self.run_case('groups')
    def test_invalid_dimension_before_requests(self): self.run_case('invalid_dim')
    def test_null_dimension_before_requests(self): self.run_case('null_dim')
    def test_null_model_before_requests(self): self.run_case('null_model')
    def test_empty_content_before_requests(self): self.run_case('empty')
    def test_argument_evaluation_failure(self): self.run_case('arg_failure')
    def test_provider_failure_no_completed_rows(self): self.run_case('failure')
    def test_response_count_mismatch(self): self.run_case('count')
    def test_lob_content_materialized(self): self.run_case('lob')
    def test_second_batch_reuses_no_prior_rows(self): self.run_case('repeat')


if __name__ == '__main__':
    unittest.main()
