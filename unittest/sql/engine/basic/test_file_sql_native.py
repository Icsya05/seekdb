"""Opt-in native FileSQL regressions; no tables or imported data are created."""
import os
import datetime
import pathlib
import tempfile
import unittest

try:
    import pymysql
except ImportError:
    pymysql = None


@unittest.skipUnless(pymysql and os.environ.get('SEEKDB_FILE_SQL_TEST') == '1',
                     'requires SEEKDB_FILE_SQL_TEST=1 and PyMySQL')
class FileSQLNative(unittest.TestCase):
    @classmethod
    def connect(cls):
        return pymysql.connect(host=os.getenv('SEEKDB_HOST', '127.0.0.1'),
                                  port=int(os.getenv('SEEKDB_PORT', '12881')),
                                  user=os.getenv('SEEKDB_USER', 'root'),
                                  password=os.getenv('SEEKDB_PASSWORD', ''),
                                  database='test', autocommit=True, read_timeout=30)

    @classmethod
    def setUpClass(cls):
        # Run only against the explicitly selected isolated test instance.
        admin = cls.connect()
        cls.addClassCleanup(admin.close)
        cls.fixture_root = os.getenv('SEEKDB_FILE_SQL_FIXTURE_DIR', '/tmp')
        with admin.cursor() as cursor:
            cursor.execute('SELECT @@global.secure_file_priv')
            previous = cursor.fetchone()[0]
            cursor.execute('SET GLOBAL secure_file_priv = %s', (cls.fixture_root,))

        def restore_policy():
            with admin.cursor() as cursor:
                cursor.execute('SET GLOBAL secure_file_priv = %s', (previous,))

        cls.addClassCleanup(restore_policy)

    def setUp(self):
        self.db = self.connect()
        self.addCleanup(self.db.close)
        self.directory = tempfile.TemporaryDirectory(prefix='seekdb-file-sql-', dir=self.fixture_root)
        self.addCleanup(self.directory.cleanup)
        self.csv = pathlib.Path(self.directory.name) / 'input.csv'
        self.csv.write_text('id,name,score,active,note\n1,Alice,1.5,true,"hello,\nworld"\n'
                            '2,Bob,2.5,false,plain\n3,Chen,,true,"say ""hi"""\n', encoding='utf-8')
        self.json = pathlib.Path(self.directory.name) / 'input.jsonl'
        self.json.write_text('{"id":1,"label":"one"}\n{"id":3,"extra":true,"label":"three"}\n', encoding='utf-8')

    def query(self, sql, args=None):
        with self.db.cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def enable(self):
        self.query('SET SESSION enable_file_sql = ON')

    def test_default_disabled(self):
        self.assertEqual(((0,),), self.query('SELECT @@session.enable_file_sql'))
        with self.assertRaises(pymysql.MySQLError) as error:
            self.query('SELECT * FROM read_csv(%s)', (str(self.csv),))
        self.assertEqual(1235, error.exception.args[0])

    def test_typed_projection_and_source_ordinals(self):
        self.enable()
        self.assertEqual((('Bob', 2, 2.5, 0), ('Chen', 3, None, 1)),
                         self.query('SELECT f.name,f.id,f.score,f.active FROM read_csv(%s) f WHERE id>=2 ORDER BY id', (str(self.csv),)))

    def test_quoted_multiline_and_escape(self):
        self.enable()
        self.assertEqual((('hello,\nworld',), ('say "hi"',)),
                         self.query('SELECT note FROM read_csv(%s) WHERE id IN (1,3) ORDER BY id', (str(self.csv),)))

    def test_count_with_no_referenced_columns(self):
        self.enable()
        self.assertEqual(((3,),), self.query('SELECT COUNT(*) FROM read_csv(%s)', (str(self.csv),)))

    def test_native_aggregate_having_limit(self):
        self.enable()
        rows = self.query('SELECT active,COUNT(*),SUM(score) FROM read_csv(%s,header=true,sample_size=-1) GROUP BY active HAVING COUNT(*)>=1 ORDER BY active LIMIT 1', (str(self.csv),))
        self.assertEqual(0, rows[0][0])
        self.assertEqual(1, rows[0][1])
        self.assertEqual(2.5, float(rows[0][2]))

    def test_native_inner_join(self):
        self.enable()
        self.assertEqual(((1, 'Alice'), (2, 'Bob'), (3, 'Chen')),
                         self.query('SELECT a.id,b.name FROM read_csv(%s) a INNER JOIN read_csv(%s) b ON a.id=b.id ORDER BY a.id', (str(self.csv), str(self.csv))))

    def test_native_left_cross_format_join(self):
        self.enable()
        self.assertEqual(((1, 'one'), (2, None), (3, 'three')),
                         self.query('SELECT a.id,b.label FROM read_csv(%s) a LEFT JOIN read_ndjson(%s) b ON a.id=b.id ORDER BY a.id', (str(self.csv), str(self.json))))

    def test_jsonl_union_missing_fields(self):
        self.enable()
        self.assertEqual(((1, None, 'one'), (3, 1, 'three')),
                         self.query("SELECT id,extra,label FROM read_json(%s,format='newline_delimited') ORDER BY id", (str(self.json),)))

    def test_full_schema_scan_late_widening(self):
        self.csv.write_text('value\n1\n2\nlate text\n', encoding='utf-8')
        self.enable()
        self.assertEqual((('1',), ('2',), ('late text',)), self.query('SELECT * FROM read_csv(%s)', (str(self.csv),)))

    def test_multiple_batches(self):
        self.csv.write_text('id,name\n' + ''.join(f'{i},row{i}\n' for i in range(2057)), encoding='utf-8')
        self.enable()
        self.assertEqual(((2057, 2114596),), self.query('SELECT COUNT(*),SUM(id) FROM read_csv(%s)', (str(self.csv),)))

    def test_dates_and_null_only_columns(self):
        self.csv.write_text('day,created,empty\n2026-08-01,2026-08-01 10:11:12,\n2026-08-02,2026-08-02T10:11:12,\n', encoding='utf-8')
        self.enable()
        rows = self.query('SELECT day,created,empty FROM read_csv(%s) ORDER BY day', (str(self.csv),))
        self.assertEqual((datetime.date(2026, 8, 1), datetime.datetime(2026, 8, 1, 10, 11, 12), None), rows[0])

    def test_duplicate_header_and_options(self):
        self.csv.write_text('id;id;note\n1;2;NULL\n', encoding='utf-8')
        self.enable()
        self.assertEqual(((2, 1, None),), self.query("SELECT id__2,id,note FROM read_csv(%s,delimiter=';',nullstr='NULL') AS f", (str(self.csv),)))

    def test_rescan(self):
        self.enable()
        rows = self.query('SELECT /*+ USE_NL(b) */ a.id,b.id FROM read_csv(%s) a CROSS JOIN read_csv(%s) b WHERE a.id<3 AND b.id<3 ORDER BY a.id,b.id', (str(self.csv), str(self.csv)))
        self.assertEqual(((1, 1), (1, 2), (2, 1), (2, 2)), rows)

    def test_repeated_sql_rebinds_changed_file(self):
        self.enable()
        sql = 'SELECT COUNT(*) FROM read_csv(%s)'
        self.assertEqual(((3,),), self.query(sql, (str(self.csv),)))
        self.csv.write_text('id\n4\n', encoding='utf-8')
        self.assertEqual(((1,),), self.query(sql, (str(self.csv),)))

    def test_reject_sample_and_nested_json(self):
        self.enable()
        with self.assertRaises(pymysql.MySQLError) as error:
            self.query('SELECT * FROM read_csv(%s,sample_size=10)', (str(self.csv),))
        self.assertEqual(1235, error.exception.args[0])
        self.json.write_text('{"nested":{"id":1}}\n', encoding='utf-8')
        with self.assertRaises(pymysql.MySQLError):
            self.query('SELECT * FROM read_json(%s)', (str(self.json),))

    def test_ordinary_sql_unchanged(self):
        self.assertEqual(((3,),), self.query('SELECT 1+2'))


if __name__ == '__main__':
    unittest.main()
