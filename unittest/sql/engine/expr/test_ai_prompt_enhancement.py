"""Opt-in SQL regressions; requires PyMySQL and a running seekdb server.

Run: SEEKDB_PROMPT_TEST=1 python -m unittest discover -s unittest/sql/engine/expr -p test_ai_prompt_enhancement.py -v
SEEKDB_HOST, SEEKDB_PORT, SEEKDB_USER and SEEKDB_PASSWORD select the instance.
The tests create uniquely named tables and mock model metadata, then clean them up.
"""

import json
import os
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import pymysql
except ImportError:
    pymysql = None


class EchoCompletionHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        content = request["messages"][-1]["content"]
        response = json.dumps({"choices": [{"message": {"content": content}}]},
                              ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get("SEEKDB_PROMPT_TEST") == "1",
                     "requires PyMySQL and SEEKDB_PROMPT_TEST=1")
class PromptSQLTestCase(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get("SEEKDB_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_PORT", "2881")),
            user=os.environ.get("SEEKDB_USER", "root"),
            password=os.environ.get("SEEKDB_PASSWORD", ""),
            database="test", charset="utf8mb4", autocommit=True,
            read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)

    def scalar(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchone()[0]


class PromptConstructionTest(PromptSQLTestCase):
    def test_json_array_and_object_preserve_structure(self):
        result = json.loads(self.scalar(
            "select ai_prompt('{0}/{1}', json_array(1,true), json_object('k',2))"))
        self.assertEqual({"template": "{0}/{1}", "args": [[1, True], {"k": 2}]}, result)

    def test_text_and_longtext_read_content(self):
        table = "prompt_lob_" + uuid.uuid4().hex[:12]
        self.addCleanup(self.drop_table, table)
        with self.connection.cursor() as cursor:
            cursor.execute("create table " + table + "(t longtext, a text)")
            cursor.execute("insert into " + table + " values(%s,%s)", ("你好 {0}", "世界\\\"\n"))
        result = json.loads(self.scalar("select ai_prompt(t,a) from " + table))
        self.assertEqual({"template": "你好 {0}", "args": ["世界\\\"\n"]}, result)

    def drop_table(self, table):
        with self.connection.cursor() as cursor:
            cursor.execute("drop table if exists " + table)


class PromptRenderingTest(PromptSQLTestCase):
    def setUp(self):
        super().setUp()
        self.mock = ThreadingHTTPServer(("127.0.0.1", 0), EchoCompletionHandler)
        self.thread = threading.Thread(target=self.mock.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_mock)
        self.model = "prompt_regression_" + uuid.uuid4().hex[:12]
        self.endpoint = self.model + "_ep"
        with self.connection.cursor() as cursor:
            cursor.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)", (
                self.model, json.dumps({"type": "completion", "model_name": "echo"})))
            self.addCleanup(self.drop_model)
            cursor.execute("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (
                self.endpoint, json.dumps({"ai_model_name": self.model, "scope": "all",
                    "url": "http://127.0.0.1:%d/v1/chat/completions" % self.mock.server_port,
                    "access_key": "local-mock", "provider": "openai"})))
            self.addCleanup(self.drop_endpoint)

    def stop_mock(self):
        self.mock.shutdown()
        self.mock.server_close()
        self.thread.join(timeout=5)

    def drop_model(self):
        with self.connection.cursor() as cursor:
            cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (self.model,))

    def drop_endpoint(self):
        with self.connection.cursor() as cursor:
            cursor.execute("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (self.endpoint,))

    def test_repeated_long_argument(self):
        value = "abcdefghij" * 100
        result = self.scalar("select ai_complete(%s,ai_prompt('{0}/{0}/{0}',%s))", (self.model, value))
        self.assertEqual(value + "/" + value + "/" + value, result)

    def test_single_placeholder_control(self):
        result = self.scalar("select ai_complete(%s,ai_prompt('{0}','x'))", (self.model,))
        self.assertEqual("x", result)

    def test_nested_prompt_and_json_render(self):
        result = self.scalar("select ai_complete(%s,ai_prompt('outer {0} {1}',"
                             "ai_prompt('inner {0}','世界'),json_array(1,2)))", (self.model,))
        self.assertTrue(result.startswith('outer inner 世界 '), result)
        self.assertEqual([1, 2], json.loads(result[len('outer inner 世界 '):]))

    def test_unicode_and_escapes_render(self):
        value = "你好\\\"\n世界"
        result = self.scalar("select ai_complete(%s,ai_prompt('{0}/{0}',%s))", (self.model, value))
        self.assertEqual(value + "/" + value, result)

    def test_huge_index_rejected(self):
        for index in ("99999999999999999999999999999999999999", "18446744073709551616", "1"):
            with self.subTest(index=index):
                with self.assertRaises(pymysql.MySQLError) as caught:
                    self.scalar("select ai_complete(%s,ai_prompt(%s,'x'))", (self.model, "{" + index + "}"))
                self.assertEqual(1210, caught.exception.args[0])
                self.assertEqual(1, self.scalar("select 1"))

    def test_malformed_outer_envelope_rejected(self):
        expressions = (
            "json_object('template',json_array(1),'args',json_array('x'))",
            "json_object('template','{0}','args',null)",
            "json_object('template',1,'args',json_array('x'))",
            "json_extract('[1]','$[0]')",
        )
        for expression in expressions:
            with self.subTest(expression=expression):
                with self.assertRaises(pymysql.MySQLError) as caught:
                    self.scalar("select ai_complete(%s," + expression + ")", (self.model,))
                self.assertEqual(1210, caught.exception.args[0])
                self.assertEqual(1, self.scalar("select 1"))


if __name__ == "__main__":
    unittest.main()
