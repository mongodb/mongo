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

from helper import simulate_crash_restart
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable07.py
# Test the rollback to stable operation performs as expected following a server crash and
# recovery. Verify that
#   (a) the on-disk value is replaced by the correct value from the history store, and
#   (b) newer updates are removed.
class test_rollback_to_stable07(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=5MB,statistics=(all),verbose=(rts:5)'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable07"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
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
        self.large_updates(uri, value_d, ds, nrows, self.prepare, 20)
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 30)
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 40)
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 50)

        # Verify data is visible and correct.
        self.check(value_d, uri, nrows, None, 21 if self.prepare else 20)
        self.check(value_c, uri, nrows, None, 31 if self.prepare else 30)
        self.check(value_b, uri, nrows, None, 41 if self.prepare else 40)
        self.check(value_a, uri, nrows, None, 51 if self.prepare else 50)

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Perform additional updates.
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 60)
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 70)
        self.large_updates(uri, value_d, ds, nrows, self.prepare, 80)

        # Checkpoint to ensure the data is flushed to disk.
        self.session.checkpoint()

        # Verify additional update data is visible and correct.
        self.check(value_b, uri, nrows, None, 61 if self.prepare else 60)
        self.check(value_c, uri, nrows, None, 71 if self.prepare else 70)
        self.check(value_d, uri, nrows, None, 81 if self.prepare else 80)

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_b, uri, nrows, None, 40)
        self.check(value_b, uri, nrows, None, 80)
        self.check(value_c, uri, nrows, None, 30)
        self.check(value_d, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreaterEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows * 4)

        # Simulate another server crash and restart.
        simulate_crash_restart(self, "RESTART", "RESTART2")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_b, uri, nrows, None, 40)
        self.check(value_b, uri, nrows, None, 80)
        self.check(value_c, uri, nrows, None, 30)
        self.check(value_d, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreaterEqual(pages_visited, 0)
        self.assertEqual(upd_aborted, 0)
        self.assertEqual(hs_removed, 0)

if __name__ == '__main__':
    wttest.run()
