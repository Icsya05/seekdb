"""Opt-in AI_EMBED scan batching against real SQL and a local HTTP provider.

Run with SEEKDB_EMBED_BATCH_TEST=1 and SEEKDB_HOST/PORT/USER/PASSWORD.
Creates and removes only randomly named test tables, models, and endpoints.
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


class BatchHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        dim = body.get('dimensions', 2)
        data = [{'index': i, 'embedding': [float(text.split(':', 1)[0])] + [0.5] * (dim - 1)}
                for i, text in enumerate(body['input'])]
        if self.server.mode == 'count':
            data.pop()
        elif self.server.mode == 'dimension':
            data[0]['embedding'].append(1.0)
        elif self.server.mode == 'duplicate' and len(data) > 1:
            data[1]['index'] = data[0]['index']
        payload = json.dumps({'data': list(reversed(data))}).encode()
        self.send_response(503 if self.server.mode == 'failure' else 200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@unittest.skipUnless(pymysql is not None and os.environ.get('SEEKDB_EMBED_BATCH_TEST') == '1',
                     'requires PyMySQL and SEEKDB_EMBED_BATCH_TEST=1')
class EmbedBatchSQL(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get('SEEKDB_HOST', '127.0.0.1'), port=int(os.environ.get('SEEKDB_PORT', '2881')),
            user=os.environ.get('SEEKDB_USER', 'root'), password=os.environ.get('SEEKDB_PASSWORD', ''),
            database='test', charset='utf8mb4', autocommit=True, read_timeout=30, write_timeout=30)
        self.addCleanup(self.connection.close)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), BatchHandler)
        self.server.requests = []
        self.server.mode = 'normal'
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        suffix = uuid.uuid4().hex[:12]
        self.table = 'embed_batch_t_' + suffix
        self.model = self.create_model('embed_batch_m_' + suffix)
        self.execute(f'CREATE TABLE {self.table} (id INT PRIMARY KEY, content LONGTEXT, dimension INT, model VARCHAR(128))')
        self.addCleanup(self.execute, f'DROP TABLE IF EXISTS {self.table}')
        self.insert([(i, f'{i}:中文', 2, self.model) for i in range(1, 9)])

    def create_model(self, name):
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                     (name, json.dumps({'type': 'dense_embedding', 'model_name': name})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (name,))
        endpoint = name + '_ep'
        self.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                     (endpoint, json.dumps({'ai_model_name': name, 'scope': 'all', 'provider': 'openai',
                                            'access_key': 'mock-key',
                                            'url': f'http://127.0.0.1:{self.server.server_port}/v1/embeddings'})))
        self.addCleanup(self.execute, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (endpoint,))
        return name

    def execute(self, sql, args=None):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def insert(self, rows):
        with self.connection.cursor() as cursor:
            cursor.executemany(f'INSERT INTO {self.table} VALUES (%s,%s,%s,%s)', rows)

    def assert_vectors(self, rows, dimension=2):
        for row_id, value in rows:
            self.assertEqual([float(row_id)] + [0.5] * (dimension - 1), json.loads(value))

    def requested_inputs(self):
        return [text for request in self.server.requests for text in request['input']]

    def scan(self, suffix='', dimension=''):
        return self.execute(f'SELECT id,AI_EMBED(%s,content{dimension}) FROM {self.table} {suffix}', (self.model,))

    def test_scan_one_request_and_reversed_response_mapping(self):
        rows = self.scan()
        self.assertEqual(list(range(1, 9)), [row[0] for row in rows])
        self.assert_vectors(rows)
        self.assertEqual(1, len(self.server.requests), 'a scan batch must issue one multi-input request')
        self.assertEqual(8, len(self.server.requests[0]['input']))

    def test_json_length_wrapper_preserves_child_batching(self):
        rows = self.execute(f'SELECT id,JSON_LENGTH(AI_EMBED(%s,content)) FROM {self.table}', (self.model,))
        self.assertEqual([(i, 2) for i in range(1, 9)], list(rows))
        self.assertEqual(1, len(self.server.requests), 'JSON_LENGTH must evaluate its child as a batch')
        self.assertEqual(8, len(self.server.requests[0]['input']))

    def test_json_length_path_wrapper_null_filter_limit(self):
        self.execute(f'UPDATE {self.table} SET content=NULL WHERE id=3')
        rows = self.execute(f"SELECT id,JSON_LENGTH(AI_EMBED(%s,content,3),'$') "
                            f'FROM {self.table} WHERE MOD(id,2)=1 LIMIT 3', (self.model,))
        self.assertEqual([(1, 3), (3, None), (5, 3)], list(rows))
        self.assertEqual(1, len(self.server.requests))
        self.assertEqual(['1:中文', '5:中文'], self.requested_inputs())

    def test_json_length_ordinary_text_and_path_semantics(self):
        documents = ['[1,2]', '{"a":[1,2,3]}', None, '{}', '[]', '"中文"', 'true', '{"a":null}']
        with self.connection.cursor() as cursor:
            cursor.executemany(f'UPDATE {self.table} SET content=%s WHERE id=%s',
                               [(document, i) for i, document in enumerate(documents, 1)])
        rows = self.execute(f"SELECT id,JSON_LENGTH(content),JSON_LENGTH(content,'$.a') FROM {self.table}")
        self.assertEqual([(1, 2, None), (2, 1, 3), (3, None, None), (4, 0, None),
                          (5, 0, None), (6, 1, None), (7, 1, None), (8, 1, 1)], list(rows))
        self.assertEqual([], self.server.requests)

    def test_null_inputs_excluded_and_rows_preserved(self):
        self.execute(f'UPDATE {self.table} SET content=NULL WHERE id IN (2,5)')
        rows = self.scan()
        self.assertEqual([2, 5], [row[0] for row in rows if row[1] is None])
        self.assert_vectors([row for row in rows if row[1] is not None])
        self.assertEqual(['1:中文', '3:中文', '4:中文', '6:中文', '7:中文', '8:中文'], self.requested_inputs())
        self.assertEqual(1, len(self.server.requests))

    def test_all_null_no_request(self):
        self.execute(f'UPDATE {self.table} SET content=NULL')
        self.assertEqual([(i, None) for i in range(1, 9)], list(self.scan()))
        self.assertEqual([], self.server.requests)

    def test_filter_skips_invalid_rows(self):
        self.execute(f"UPDATE {self.table} SET content='' WHERE MOD(id,2)=0")
        rows = self.scan('WHERE MOD(id,2)=1')
        self.assert_vectors(rows)
        self.assertEqual(['1:中文', '3:中文', '5:中文', '7:中文'], self.requested_inputs())
        self.assertEqual(1, len(self.server.requests))

    def test_limit_requests_only_consumed_rows(self):
        rows = self.scan('LIMIT 3')
        self.assertEqual([1, 2, 3], [row[0] for row in rows])
        self.assert_vectors(rows)
        self.assertEqual(['1:中文', '2:中文', '3:中文'], self.requested_inputs())
        self.assertEqual(1, len(self.server.requests))

    def test_multiple_engine_batches_keep_row_mapping(self):
        self.insert([(i, f'{i}:batch', 2, self.model) for i in range(9, 2058)])
        rows = self.scan()
        self.assertEqual(2057, len(rows))
        self.assert_vectors(rows)
        self.assertGreater(len(self.server.requests), 1)
        self.assertTrue(all(len(req['input']) > 1 for req in self.server.requests[:-1]))
        self.assertEqual(2057, len(self.requested_inputs()))
        self.assertEqual(2057, len(set(self.requested_inputs())))

    def test_mixed_dimensions_are_grouped(self):
        self.execute(f'UPDATE {self.table} SET dimension=3 WHERE MOD(id,2)=0')
        rows = self.scan(dimension=',dimension')
        for row_id, value in rows:
            self.assert_vectors([(row_id, value)], 3 if row_id % 2 == 0 else 2)
        self.assertEqual(2, len(self.server.requests))
        self.assertEqual([2, 3], sorted(req['dimensions'] for req in self.server.requests))
        self.assertEqual([4, 4], sorted(len(req['input']) for req in self.server.requests))

    def test_mixed_models_are_grouped(self):
        other = self.create_model(self.model + '_other')
        self.execute(f'UPDATE {self.table} SET model=%s WHERE MOD(id,2)=0', (other,))
        rows = self.execute(f'SELECT id,AI_EMBED(model,content,dimension) FROM {self.table}')
        self.assert_vectors(rows)
        self.assertEqual(2, len(self.server.requests))
        self.assertEqual([self.model, other], sorted(req['model'] for req in self.server.requests))

    def test_invalid_dimension_before_http(self):
        self.execute(f'UPDATE {self.table} SET dimension=0 WHERE id=8')
        with self.assertRaises(pymysql.MySQLError):
            self.scan(dimension=',dimension')
        self.assertEqual([], self.server.requests)

    def test_provider_failure_aborts_query(self):
        self.server.mode = 'failure'
        with self.assertRaises(pymysql.MySQLError):
            self.scan()
        # The existing HTTP client retries failed requests; every attempt must
        # still contain the whole compatible batch.
        self.assertGreaterEqual(len(self.server.requests), 1)
        self.assertTrue(all(len(request['input']) == 8 for request in self.server.requests))

    def test_invalid_count_aborts_query(self):
        self.server.mode = 'count'
        with self.assertRaises(pymysql.MySQLError):
            self.scan()

    def test_invalid_dimension_response_aborts_query(self):
        self.server.mode = 'dimension'
        with self.assertRaises(pymysql.MySQLError):
            self.scan(dimension=',2')

    def test_duplicate_response_index_aborts_query(self):
        self.server.mode = 'duplicate'
        with self.assertRaises(pymysql.MySQLError):
            self.scan()


if __name__ == '__main__':
    unittest.main()
