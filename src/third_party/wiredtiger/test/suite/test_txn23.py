#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# test_txn23.py
#   Transactions: ensure read timestamp is not cleared under cache pressure
#

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_txn23(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'
    conn_config = 'cache_size=5MB'

    key_format_values = [
        ('integer-row', dict(key_format='i')),
        ('column', dict(key_format='r')),
    ]
    scenarios = make_scenarios(key_format_values)

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records.
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, ds, nrows, read_ts):
        for i in range(1, nrows + 1):
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
            cursor = self.session.open_cursor(uri)
            self.assertEqual(cursor[ds.key(i)], check_value)
            cursor.close()
            self.session.commit_transaction()

    def test_txn(self):
        nrows = 2000

        # Create a table.
        uri_1 = "table:txn23_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format="S")
        ds_1.populate()

        # Create another table.
        uri_2 = "table:txn23_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format="S")
        ds_2.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        value_c = "ccccc" * 100
        value_d = "ddddd" * 100

        # Perform several updates.
        self.large_updates(uri_1, value_d, ds_1, nrows, 20)
        self.large_updates(uri_1, value_c, ds_1, nrows, 30)
        self.large_updates(uri_1, value_b, ds_1, nrows, 40)
        self.large_updates(uri_1, value_a, ds_1, nrows, 50)

        self.large_updates(uri_2, value_d, ds_2, nrows, 20)
        self.large_updates(uri_2, value_c, ds_2, nrows, 30)
        self.large_updates(uri_2, value_b, ds_2, nrows, 40)
        self.large_updates(uri_2, value_a, ds_2, nrows, 50)

        # Verify data is visible and correct.
        self.check(value_d, uri_1, ds_1, nrows, 20)
        self.check(value_c, uri_1, ds_1, nrows, 30)
        self.check(value_b, uri_1, ds_1, nrows, 40)
        self.check(value_a, uri_1, ds_1, nrows, 50)

        self.check(value_d, uri_2, ds_2, nrows, 20)
        self.check(value_c, uri_2, ds_2, nrows, 30)
        self.check(value_b, uri_2, ds_2, nrows, 40)
        self.check(value_a, uri_2, ds_2, nrows, 50)
