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

import threading, time
import wttest
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint37.py
#
# Test that reconciliation removes obsolete updates on the page.
class test_checkpoint37(wttest.WiredTigerTestCase):
    conn_config = 'cache_eviction_controls=[skip_update_obsolete_check=true]'

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]

    scenarios = make_scenarios(format_values)

    def large_updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def check(self, ds, nrows, value):
        cursor = self.session.open_cursor(ds.uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        cursor.close()

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_checkpoint(self):
        uri = 'table:checkpoint37'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
            value_e = 101
        else:
            value_a = "aaaaa" * 10
            value_b = "bbbbb" * 10
            value_c = "ccccc" * 10
            value_d = "ddddd" * 10
            value_e = "eeeee" * 10

        # Write some initial data.
        self.large_updates(ds.uri, ds, nrows, value_a, 5)

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Checkpoint and reopen the connection to read from the on-disk version.
        self.session.checkpoint()
        self.reopen_conn()

        # Add updates to each key to check whether they free on reconciliation.
        self.large_updates(ds.uri, ds, nrows, value_b, 10)
        prev_bytes_in_use = self.get_stat(stat.conn.cache_bytes_inuse)

        self.large_updates(ds.uri, ds, nrows, value_c, 20)

        # Pin oldest and stable timestamps to 20.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
            ',stable_timestamp=' + self.timestamp_str(20))

        # Checkpoint.
        self.session.checkpoint()
        bytes_in_use = self.get_stat(stat.conn.cache_bytes_inuse)
        self.assertLess(bytes_in_use, prev_bytes_in_use * 2)

        # Another set of updates.
        self.large_updates(ds.uri, ds, nrows, value_d, 30)

        # Pin oldest and stable timestamps to 30.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30) +
            ',stable_timestamp=' + self.timestamp_str(30))

        # Checkpoint.
        self.session.checkpoint()
        bytes_in_use = self.get_stat(stat.conn.cache_bytes_inuse)
        self.assertLess(bytes_in_use, prev_bytes_in_use * 2)

        # Another set of updates.
        self.large_updates(ds.uri, ds, nrows, value_e, 40)

        # Pin oldest and stable timestamps to 40.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(40) +
            ',stable_timestamp=' + self.timestamp_str(40))

        # Checkpoint.
        self.session.breakpoint()
        self.session.checkpoint()
        bytes_in_use = self.get_stat(stat.conn.cache_bytes_inuse)
        self.assertLess(bytes_in_use, prev_bytes_in_use * 2)
        self.assertGreater(self.get_stat(stat.conn.cache_obsolete_updates_removed), 0)

if __name__ == '__main__':
    wttest.run()
