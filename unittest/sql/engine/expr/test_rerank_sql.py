"""Opt-in SQL rerank regression tests against a newly built seekdb server.

SEEKDB_RERANK_TEST=1 enables tests; SEEKDB_HOST/PORT/USER/PASSWORD select
the server. Only randomly named test metadata is created and removed.
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


class RerankHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        docs = body['documents']
        results = [{'index': i, 'relevance_score': int(doc.split(':')[0])}
                   for i, doc in enumerate(docs)]
        # Deliberately unsorted and all equal scores must retain original order.
        results.reverse()
        mode = self.server.mode
        if mode == 'short':
            results.pop()
        elif mode == 'duplicate':
            results[-1]['index'] = results[0]['index']
        elif mode == 'range':
            results[0]['index'] = len(docs)
        elif mode == 'fractional':
            results[0]['index'] = 0.5
        elif mode == 'string_score':
            results[0]['relevance_score'] = 'invalid'
        elif mode == 'container':
            results = {}
        payload = json.dumps({'results': results}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get('SEEKDB_RERANK_TEST') == '1',
                     'requires PyMySQL and SEEKDB_RERANK_TEST=1')
class RerankSQL(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'), password=os.environ.get('SEEKDB_PASSWORD', ''),
            database='test', charset='utf8mb4', autocommit=True, read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), RerankHandler)
        self.server.requests, self.server.mode = [], 'valid'
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.model, self.endpoint = 'rerank_model_' + uuid.uuid4().hex[:12], 'rerank_ep_' + uuid.uuid4().hex[:12]
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                     (self.model, json.dumps({'type': 'rerank', 'model_name': 'local-test-model'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                     (self.endpoint, json.dumps({'ai_model_name': self.model, 'scope': 'all',
                                                'provider': 'siliconflow', 'access_key': 'mock-key',
                                                'url': f'http://127.0.0.1:{self.server.server_port}/rerank'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    def execute(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def rerank(self, documents, extra='', extra_args=()):
        rows = self.execute('SELECT AI_RERANK(%s,%s,%s' + extra + ')',
                            (self.model, 'query 中文', json.dumps(documents), *extra_args))
        return json.loads(rows[0][0])

    def test_string_array_and_null_key(self):
        for extra in ('', ',NULL'):
            with self.subTest(extra=extra):
                result = self.rerank(['1:甲', '3:乙', '2:丙'], extra)
                self.assertEqual([1, 2, 0], [r['index'] for r in result])

    def test_batch_mapping_stable_ties(self):
        docs = ['5:item' + str(i) for i in range(43)]
        result = self.rerank(docs)
        self.assertEqual(list(range(43)), [r['index'] for r in result])
        self.assertEqual([20, 20, 3], [len(r['documents']) for r in self.server.requests])

    def test_objects_text_key_and_ids(self):
        docs = [{'id': 'original-' + str(i), 'text': '5:文本', '_model_score': -1} for i in range(43)]
        result = self.rerank(docs, ',%s', ('text',))
        self.assertEqual(['original-' + str(i) for i in range(43)], [r['id'] for r in result])
        self.assertEqual([5] * 43, [r['_model_score'] for r in result])
        self.assertEqual([20, 20, 3], [len(r['documents']) for r in self.server.requests])

    def test_invalid_documents_rejected_before_http(self):
        for documents, extra, args in ((['text', {}], '', ()), ([{'text': None}], ',%s', ('text',)),
                                       ([{'other': 'value'}], ',%s', ('text',)), ([], '', ())):
            with self.subTest(documents=documents):
                with self.assertRaises(pymysql.MySQLError):
                    self.rerank(documents, extra, args)
        self.assertEqual([], self.server.requests)

    def test_malformed_results_rejected(self):
        for mode in ('short', 'duplicate', 'range', 'fractional', 'string_score', 'container'):
            self.server.mode = mode
            with self.subTest(mode=mode):
                with self.assertRaises(pymysql.MySQLError):
                    self.rerank(['1:first', '2:second'])


if __name__ == '__main__':
    unittest.main()
