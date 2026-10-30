"""Opt-in real SQL completion configuration/response tests with local HTTP.

Set SEEKDB_COMPLETION_TEST=1 and the usual SEEKDB_HOST/PORT/USER/PASSWORD.
Each test creates and removes only its own randomly named model and endpoint.
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


class CompletionHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        mode = self.server.mode
        if mode in ('array_root', 'scalar_root', 'null_root', 'invalid_json'):
            payload = {'array_root': b'[]', 'scalar_root': b'"text"',
                       'null_root': b'null', 'invalid_json': b'{'}[mode]
        else:
            content = {'empty': '', 'nonstring': {'text': 'invalid'}}.get(mode, 'Unicode 中文 completion')
            payload = json.dumps({'choices': [{'message': {'content': content}}]}, ensure_ascii=False).encode()
        self.send_response(201 if mode == 'created' else 200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get('SEEKDB_COMPLETION_TEST') == '1',
                     'requires PyMySQL and SEEKDB_COMPLETION_TEST=1')
class CompletionSQL(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'), port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'), password=os.environ.get('SEEKDB_PASSWORD', ''),
            database='test', charset='utf8mb4', autocommit=True, read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), CompletionHandler)
        self.server.mode = 'valid'
        self.server.requests = []
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.model = 'complete_model_' + uuid.uuid4().hex[:12]
        self.endpoint = 'complete_ep_' + uuid.uuid4().hex[:12]
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                     (self.model, json.dumps({'type': 'completion', 'model_name': 'local-test-model'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                     (self.endpoint, json.dumps({'ai_model_name': self.model, 'scope': 'all', 'provider': 'openai',
                                                'access_key': 'mock-key',
                                                'url': f'http://127.0.0.1:{self.server.server_port}/v1/chat/completions'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    def execute(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def completion(self, config_sql='', args=()):
        # Preserve existing output serialization; this test concerns config/validation.
        rows = self.execute('SELECT JSON_UNQUOTE(AI_COMPLETE(%s,%s' + config_sql + '))',
                            (self.model, 'prompt 中文', *args))
        self.assertEqual((('Unicode 中文 completion',),), rows)

    def test_omitted_config(self):
        self.completion()

    def test_null_config(self):
        self.completion(',NULL')

    def test_native_json_config(self):
        self.completion(",JSON_OBJECT('temperature',0.2,'max_tokens',16)")
        self.assertEqual(0.2, self.server.requests[-1]['temperature'])
        self.assertEqual(16, self.server.requests[-1]['max_tokens'])

    def test_text_config(self):
        self.completion(',%s', ('{"temperature":0.4,"max_tokens":21}',))
        self.assertEqual(0.4, self.server.requests[-1]['temperature'])
        self.assertEqual(21, self.server.requests[-1]['max_tokens'])

    def test_array_config_rejected_before_request(self):
        with self.assertRaises(pymysql.MySQLError):
            self.completion(',JSON_ARRAY(1,2)')
        self.assertEqual([], self.server.requests)

    def test_empty_response_rejected(self):
        self.server.mode = 'empty'
        with self.assertRaises(pymysql.MySQLError):
            self.execute('SELECT AI_COMPLETE(%s,%s)', (self.model, 'prompt'))

    def test_object_response_rejected(self):
        self.server.mode = 'nonstring'
        with self.assertRaises(pymysql.MySQLError):
            self.execute('SELECT AI_COMPLETE(%s,%s)', (self.model, 'prompt'))

    def test_non_object_or_invalid_json_envelope_rejected_without_losing_connection(self):
        for mode in ('array_root', 'scalar_root', 'null_root', 'invalid_json'):
            with self.subTest(mode=mode):
                self.server.mode = mode
                with self.assertRaises(pymysql.MySQLError):
                    self.execute('SELECT AI_COMPLETE(%s,%s)', (self.model, 'prompt'))
                self.assertEqual(((1,),), self.execute('SELECT 1'))
                self.server.mode = 'valid'
                self.completion()

    def test_successful_201_response(self):
        self.server.mode = 'created'
        self.completion()


if __name__ == '__main__':
    unittest.main()
