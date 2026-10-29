"""Opt-in real SQL Ollama regressions, with a local HTTP model boundary.

Run SEEKDB_OLLAMA_TEST=1 python -m unittest discover -s unittest/sql/engine/expr
-p test_ollama_provider.py -v. Connection variables match the prompt SQL tests.
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


class NativeOllamaHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, dict(self.headers), body))
        if self.path == "/api/generate":
            response = {"response": body["prompt"]}
        elif self.path == "/api/embed":
            response = {"embeddings": [[0.25, -0.5, 1.0] for _ in body["input"]]}
        else:
            self.send_error(404)
            return
        payload = json.dumps(response, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get("SEEKDB_OLLAMA_TEST") == "1",
                     "requires PyMySQL and SEEKDB_OLLAMA_TEST=1")
class OllamaSQLTest(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get("SEEKDB_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_PORT", "2881")),
            user=os.environ.get("SEEKDB_USER", "root"),
            password=os.environ.get("SEEKDB_PASSWORD", ""),
            database="test", charset="utf8mb4", autocommit=True,
            read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), NativeOllamaHandler)
        self.server.requests = []
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.model = "ollama_model_" + uuid.uuid4().hex[:12]
        self.endpoint = "ollama_ep_" + uuid.uuid4().hex[:12]

    def execute(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def drop_metadata(self):
        self.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (self.model,))
        self.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (self.endpoint,))

    def create(self, provider="ollama", key="", kind="completion", omit_key=False):
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                     (self.model, json.dumps({"type": kind, "model_name": "local-test-model"})))
        path = "/api/generate" if kind == "completion" else "/api/embed"
        params = {"ai_model_name": self.model, "scope": "all", "provider": provider,
                  "url": f"http://127.0.0.1:{self.server.server_port}{path}"}
        if not omit_key:
            params["access_key"] = key
        try:
            self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                         (self.endpoint, json.dumps(params)))
        except Exception:
            self.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (self.model,))
            raise
        self.addCleanup(self.drop_metadata)

    def test_endpoint_empty_key_and_case_insensitive_provider_alter(self):
        self.create()
        for provider in ("ollama", "Ollama", "OLLAMA"):
            self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                         (self.endpoint, json.dumps({"provider": provider})))
            self.assertEqual(((provider,),), self.execute(
                "SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s",
                (self.endpoint,)))

    def test_endpoint_omitted_key_is_optional_for_ollama(self):
        self.create(provider="OlLaMa", omit_key=True)

    def test_endpoint_provider_accepted_with_nonempty_key(self):
        self.create(provider="OlLaMa", key="optional-key")

    def test_alter_openai_endpoint_to_ollama_and_clear_key(self):
        self.create(provider="openai", key="initial-key")
        self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                     (self.endpoint, '{"provider":"Ollama","access_key":""}'))
        self.assertEqual((("Ollama",),), self.execute(
            "SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s",
            (self.endpoint,)))
        # Reload the persisted empty key through a later ALTER, then use it.
        self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                     (self.endpoint, '{"provider":"OLLAMA"}'))
        self.assertEqual((("still keyless",),), self.execute(
            "SELECT AI_COMPLETE(%s,%s)", (self.model, "still keyless")))
        self.assertNotIn("Authorization", self.server.requests[-1][1])

    def test_native_completion_is_non_streaming_without_authorization(self):
        self.create(provider="OlLaMa")
        self.assertEqual((("你好\\\"\ntext",),), self.execute(
            "SELECT AI_COMPLETE(%s,%s,NULL)", (self.model, "你好\\\"\ntext")))
        path, headers, body = self.server.requests[-1]
        self.assertEqual("/api/generate", path)
        self.assertIs(body["stream"], False)
        self.assertEqual("local-test-model", body["model"])
        self.assertEqual("你好\\\"\ntext", body["prompt"])
        self.assertNotIn("Authorization", headers)
        self.assertEqual("application/json", headers.get("Content-Type"))

    def test_native_embedding_protocol(self):
        self.create(provider="OLLAMA", kind="dense_embedding")
        rows = self.execute("SELECT AI_EMBED(%s,%s)", (self.model, "arbitrary input"))
        self.assertEqual([0.25, -0.5, 1.0], json.loads(rows[0][0]))
        path, headers, body = self.server.requests[-1]
        self.assertEqual("/api/embed", path)
        self.assertEqual(["arbitrary input"], body["input"])
        self.assertEqual("local-test-model", body["model"])
        self.assertNotIn("Authorization", headers)

    def test_changing_keyless_endpoint_to_openai_requires_key(self):
        self.create()
        with self.assertRaises(pymysql.MySQLError) as caught:
            self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                         (self.endpoint, '{"provider":"openai"}'))
        self.assertEqual(11114, caught.exception.args[0])


if __name__ == "__main__":
    unittest.main()
