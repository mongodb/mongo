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

import wttest
from wtdataset import SimpleDataSet
from wiredtiger import stat
from wtscenario import make_scenarios
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable03.py
# Test that rollback to stable clears the history store updates from reconciled pages.
class test_rollback_to_stable01(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, in_memory_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=4GB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable03"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
            valuec = 99
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100
            valuec = "ccccc" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri, valuea, ds, nrows, self.prepare, 10)
        # Check that all updates are seen.
        self.check(valuea, uri, nrows, None, 10)

        self.large_updates(uri, valueb, ds, nrows, self.prepare, 20)
        # Check that all updates are seen.
        self.check(valueb, uri, nrows, None, 20)

        self.large_updates(uri, valuec, ds, nrows, self.prepare, 30)
        # Check that all updates are seen.
        self.check(valuec, uri, nrows, None, 30)

        # Pin stable to timestamp 30 if prepare otherwise 20.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        # Checkpoint to ensure that all the updates are flushed to disk.
        if not self.in_memory:
            self.session.checkpoint()

        self.conn.rollback_to_stable()
        # Check that the old updates are only seen even with the update timestamp.
        self.check(valueb, uri, nrows, None, 20)
        self.check(valuea, uri, nrows, None, 10)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 1)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        if self.in_memory:
            self.assertEqual(upd_aborted, nrows)
            self.assertEqual(hs_removed, 0)
        else:
            self.assertGreaterEqual(upd_aborted + hs_removed, nrows)
        self.assertGreater(pages_visited, 0)

if __name__ == '__main__':
    wttest.run()
