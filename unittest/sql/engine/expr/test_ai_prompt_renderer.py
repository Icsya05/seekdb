"""Compile and execute the production prompt renderer with boundary doubles.

This component check does not compile the SQL engine or validate LOB/type APIs.
AI_PROMPT_SOURCE may select an unmodified source file for RED verification.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which("g++"), "requires g++")
class PromptRendererComponentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ai-prompt-renderer-")
        cls.addClassCleanup(cls.directory.cleanup)
        test_path = Path(__file__).resolve().parent
        repo = test_path.parents[3]
        source = Path(os.environ.get("AI_PROMPT_SOURCE", repo / "src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp"))
        text = source.read_text(encoding="utf-8")
        start = text.index("bool ObAIFuncPromptObjectUtils::is_valid_prompt_object")
        end = text.index("const ObString ObAIFuncModel::get_request_model_name", start)
        temporary = Path(cls.directory.name)
        (temporary / "renderer_under_test.inc").write_text(text[start:end], encoding="utf-8")
        cls.binary = temporary / "renderer-test"
        compile_result = subprocess.run(
            ["g++", "-std=c++17", "-O1", "-g", "-fsanitize=undefined",
             "-fno-sanitize-recover=all", "-I", str(temporary),
             str(test_path / "ai_prompt_renderer_harness.cpp"), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if compile_result.returncode:
            raise RuntimeError(compile_result.stdout + compile_result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_repeated_long_argument(self):
        self.run_case("repeat")

    def test_placeholder_index_overflow(self):
        self.run_case("overflow")

    def test_nested_prompt_and_json(self):
        self.run_case("nested_json")

    def test_literal_braces_and_empty_argument(self):
        self.run_case("literal_empty")

    def test_malformed_envelope(self):
        self.run_case("malformed")

    def test_missing_null_or_wrong_envelope_fields(self):
        self.run_case("malformed_args")

    def test_deep_nesting_is_bounded(self):
        self.run_case("depth")

    def test_ordinary_json_object(self):
        self.run_case("json_object")

    def test_unicode_escapes_and_embedded_nul(self):
        self.run_case("unicode_nul")

    def test_empty_result(self):
        self.run_case("empty_result")

    def test_template_without_arguments(self):
        self.run_case("no_arguments")


if __name__ == "__main__":
    unittest.main()
