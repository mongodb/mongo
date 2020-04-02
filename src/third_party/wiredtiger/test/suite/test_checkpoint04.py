#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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
# test_checkpoint04.py
# Test that the checkpoints timing statistics are populated as expected.

import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

def timestamp_str(t):
    return '%x' % t

class test_checkpoint04(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,log=(enabled),statistics=(all)'
    session_config = 'isolation=snapshot'

    def create_tables(self, ntables):
        tables = {}
        for i in range(0, ntables):
            uri = 'table:table' + str(i)
            ds = SimpleDataSet(
                self, uri, 0, key_format="i", value_format="S", config='log=(enabled=false)')
            ds.populate()
            tables[uri] = ds
        return tables

    def add_updates(self, uri, ds, value, nrows, ts):
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + timestamp_str(ts))
        cursor.close()

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_checkpoint_stats(self):
        nrows = 1000
        ntables = 50

        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        # Create many tables and perform many updates so our checkpoint stats are populated.
        value = "wired" * 100
        tables = self.create_tables(ntables)
        for uri, ds in tables.items():
            self.add_updates(uri, ds, value, nrows, 20)

        # Perform a checkpoint.
        self.session.checkpoint()

        # Update the tables.
        value = "tiger" * 100
        tables = self.create_tables(ntables)
        for uri, ds in tables.items():
            self.add_updates(uri, ds, value, nrows, 30)

        # Perform a checkpoint.
        self.session.checkpoint()

        # Check the statistics.
        self.assertEqual(self.get_stat(stat.conn.txn_checkpoint), 2)
        self.assertEqual(self.get_stat(stat.conn.txn_checkpoint_running), 0)
        self.assertEqual(self.get_stat(stat.conn.txn_checkpoint_prep_running), 0)
        self.assertLess(self.get_stat(stat.conn.txn_checkpoint_prep_min),
            self.get_stat(stat.conn.txn_checkpoint_time_min))
        self.assertLess(self.get_stat(stat.conn.txn_checkpoint_prep_max),
            self.get_stat(stat.conn.txn_checkpoint_time_max))
        self.assertLess(self.get_stat(stat.conn.txn_checkpoint_prep_recent),
            self.get_stat(stat.conn.txn_checkpoint_time_recent))
        self.assertLess(self.get_stat(stat.conn.txn_checkpoint_prep_total),
            self.get_stat(stat.conn.txn_checkpoint_time_total))

if __name__ == '__main__':
    wttest.run()
