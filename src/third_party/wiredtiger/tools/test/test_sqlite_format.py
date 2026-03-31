#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.

import os
import sqlite3
import sys
import tempfile
import unittest
import logging

# Add tools directory to sys.path so we can import py_common
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from py_common import sqlite_format


class TestSqliteFormat(unittest.TestCase):

    def _create_test_db(self):
        temp_file = tempfile.NamedTemporaryFile(delete=False, suffix='.db')
        temp_file.close()

        with sqlite3.connect(temp_file.name) as conn:
            conn.execute(
                '''
                CREATE TABLE pages (
                    table_id INTEGER NOT NULL,
                    page_id INTEGER NOT NULL,
                    lsn INTEGER NOT NULL,
                    backlink_lsn INTEGER NOT NULL,
                    base_lsn INTEGER NOT NULL,
                    flags INTEGER NOT NULL,
                    discarded INTEGER NOT NULL DEFAULT 0,
                    page_data BLOB
                )
                '''
            )
            conn.executemany(
                '''
                INSERT INTO pages (
                    table_id,
                    page_id,
                    lsn,
                    backlink_lsn,
                    base_lsn,
                    flags,
                    discarded,
                    page_data
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                ''',
                [
                    (1, 10,  1,  0,  0, 0, 0, b'base10'),
                    (1, 10,  5,  1,  1, 2, 0, b'delta10_5'),
                    (1, 10,  7,  5,  1, 2, 0, b'delta10_7'),
                    (1, 10,  9,  0,  0, 0, 0, b'base10_newer'),
                    (1, 10, 11,  9,  9, 2, 0, b'delta10_11'),
                    (1, 11, 17,  0,  0, 0, 0, b'base11'),
                    (1, 12, 20,  0,  0, 0, 0, b'base12_20'),
                    (1, 12, 25, 20,  0, 0, 0, b'base12_25'),
                    (1, 12, 30, 25,  0, 0, 0, b'base12_30'),
                    (1, 13, 35,  0,  0, 0, 0, b'discarded13_35'),
                    (1, 13, 40, 35, 35, 2, 1, b'discarded13_40'),
                ],
            )

        return temp_file.name


    def setUp(self):
        self.db_path = self._create_test_db()
        return super().setUp()


    def tearDown(self):
        if os.path.exists(self.db_path):
            os.remove(self.db_path)
        return super().tearDown()


    def test_is_sqlite3_file(self):
        self.assertTrue(sqlite_format.is_sqlite3_file(self.db_path))

        non_sqlite = tempfile.NamedTemporaryFile(delete=False)
        self.addCleanup(lambda: os.remove(non_sqlite.name)
                        if os.path.exists(non_sqlite.name) else None)

        non_sqlite.write(b'not-a-sqlite-file')
        non_sqlite.close()
        self.assertFalse(sqlite_format.is_sqlite3_file(non_sqlite.name))


    def test_page_id_latest_lsn(self):
        with self.assertLogs('py_common.sqlite_format', level=logging.WARNING) as logs:
            pages = sqlite_format.load_disagg_pages(self.db_path, page_id=10)

        self.assertEqual(len(pages), 1)
        self.assertEqual([entry.metadata.lsn for entry in pages[0]], [11, 9])
        self.assertIn('Found 2 base pages for page_id=10', logs.output[0])


    def test_page_id_specific_lsn(self):
        pages = sqlite_format.load_disagg_pages(self.db_path, page_id=10, lsn=5)

        self.assertEqual(len(pages), 1)
        self.assertEqual([entry.metadata.lsn for entry in pages[0]], [5])


    def test_page_id_single_base_no_warning(self):
        pages = sqlite_format.load_disagg_pages(self.db_path, page_id=11)

        self.assertEqual(len(pages), 1)
        self.assertEqual([entry.metadata.lsn for entry in pages[0]], [17])


    def test_without_page_id_respects_pages_limit(self):
        pages = sqlite_format.load_disagg_pages(self.db_path, pages=1)

        self.assertEqual(len(pages), 1)
        self.assertEqual([entry.metadata.page_id for entry in pages[0]], [10, 10])


    def test_lsn_without_page_id(self):
        pages = sqlite_format.load_disagg_pages(self.db_path, lsn=7)

        self.assertEqual(len(pages), 1)
        self.assertEqual(len(pages[0]), 1)
        self.assertEqual(pages[0][0].metadata.page_id, 10)
        self.assertEqual(pages[0][0].metadata.lsn, 7)


    def test_discarded_lsn_query_returns_none(self):
        with sqlite3.connect(self.db_path) as conn:
            conn.row_factory = sqlite3.Row
            row = sqlite_format._load_row_for_lsn(conn, 40)

        self.assertIsNone(row)


    def test_page_id_returns_base_page_chain(self):
        pages = sqlite_format.load_disagg_pages(self.db_path, page_id=12)

        self.assertEqual(len(pages), 1)
        self.assertEqual([page.metadata.lsn for page in pages[0]], [30, 25, 20])
        self.assertEqual([page.metadata.backlink_lsn for page in pages[0]], [25, 20, 0])
        self.assertTrue(all(not page.metadata.is_delta() for page in pages[0]))


    def test_lsn_and_page_id_mismatch(self):
        with self.assertRaises(ValueError):
            sqlite_format.load_disagg_pages(self.db_path, page_id=11, lsn=7)


if __name__ == "__main__":
    unittest.main()
