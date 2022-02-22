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

from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable06.py
# Test that rollback to stable removes all keys when the stable timestamp is earlier than
# all commit timestamps.
class test_rollback_to_stable06(test_rollback_to_stable_base):

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
        config = 'cache_size=50MB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable06"
        ds_config = ',log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100
            value_d = "ddddd" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 30)
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 40)
        self.large_updates(uri, value_d, ds, nrows, self.prepare, 50)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_c, uri, nrows, None, 40)
        self.check(value_d, uri, nrows, None, 50)

        # Checkpoint to ensure the data is flushed, then rollback to the stable timestamp.
        if not self.in_memory:
            self.session.checkpoint()
        self.conn.rollback_to_stable()

        # Check that all keys are removed.
        # (For FLCS, at least for now, they will read back as 0, meaning deleted, rather
        # than disappear.)
        self.check(value_a, uri, 0, nrows, 20)
        self.check(value_b, uri, 0, nrows, 30)
        self.check(value_c, uri, 0, nrows, 40)
        self.check(value_d, uri, 0, nrows, 50)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 1)
        self.assertEqual(keys_restored, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(keys_removed, 0)
        if self.in_memory:
            self.assertEqual(upd_aborted, nrows * 4)
            self.assertEqual(hs_removed, 0)
        else:
            self.assertGreaterEqual(upd_aborted + hs_removed + keys_removed, nrows * 4)

if __name__ == '__main__':
    wttest.run()
