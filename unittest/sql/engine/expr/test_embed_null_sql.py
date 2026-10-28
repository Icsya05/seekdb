"""Opt-in scalar AI_EMBED NULL regression tests against a real SQL server.

Set SEEKDB_EMBED_NULL_TEST=1 and SEEKDB_HOST/PORT/USER/PASSWORD as needed.
Only randomly named metadata created by each test is removed during cleanup.
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


class EmbedHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        payload = json.dumps({'data': [{'index': i, 'embedding': [0.25, 0.5]}
                                       for i in range(len(body['input']))]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get('SEEKDB_EMBED_NULL_TEST') == '1',
                     'requires PyMySQL and SEEKDB_EMBED_NULL_TEST=1')
class EmbedNullSQL(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'), port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'), password=os.environ.get('SEEKDB_PASSWORD', ''),
            database='test', charset='utf8mb4', autocommit=True, read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), EmbedHandler)
        self.server.requests = []
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.model = 'embed_null_model_' + uuid.uuid4().hex[:12]
        self.endpoint = 'embed_null_ep_' + uuid.uuid4().hex[:12]
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                     (self.model, json.dumps({'type': 'dense_embedding', 'model_name': 'local-test-model'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                     (self.endpoint, json.dumps({'ai_model_name': self.model, 'scope': 'all', 'provider': 'openai',
                                                'access_key': 'mock-key',
                                                'url': f'http://127.0.0.1:{self.server.server_port}/v1/embeddings'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    def execute(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def test_null_content(self):
        self.assertEqual(((None,),), self.execute('SELECT AI_EMBED(%s,NULL)', (self.model,)))
        self.assertEqual([], self.server.requests)

    def test_nullable_content_rowset(self):
        rows = self.execute("SELECT id,AI_EMBED(%s,content) FROM "
                            "(SELECT 1 id,'first' content UNION ALL SELECT 2,NULL UNION ALL SELECT 3,'last') t "
                            "ORDER BY id", (self.model,))
        self.assertEqual([1, 2, 3], [row[0] for row in rows])
        self.assertEqual([0.25, 0.5], json.loads(rows[0][1]))
        self.assertIsNone(rows[1][1])
        self.assertEqual([0.25, 0.5], json.loads(rows[2][1]))
        self.assertEqual(['first', 'last'], [value for request in self.server.requests for value in request['input']])

    def test_default_dimension(self):
        rows = self.execute('SELECT AI_EMBED(%s,%s)', (self.model, 'content'))
        self.assertEqual([0.25, 0.5], json.loads(rows[0][0]))
        self.assertNotIn('dimensions', self.server.requests[-1])

    def test_explicit_dimension(self):
        rows = self.execute('SELECT AI_EMBED(%s,%s,2)', (self.model, 'content'))
        self.assertEqual([0.25, 0.5], json.loads(rows[0][0]))
        self.assertEqual(2, self.server.requests[-1]['dimensions'])

    def test_nullable_integer_dimension_rejected(self):
        with self.assertRaises(pymysql.MySQLError):
            self.execute('SELECT AI_EMBED(%s,%s,CAST(NULL AS SIGNED))', (self.model, 'content'))
        self.assertEqual([], self.server.requests)

    def test_empty_content_rejected(self):
        with self.assertRaises(pymysql.MySQLError):
            self.execute('SELECT AI_EMBED(%s,%s)', (self.model, ''))
        self.assertEqual([], self.server.requests)


if __name__ == '__main__':
    unittest.main()
