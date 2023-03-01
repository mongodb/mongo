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

# test_rollback_to_stable12.py
# Test the rollback to stable operation skipping subtrees in during tree walk.
class test_rollback_to_stable12(test_rollback_to_stable_base):

    # This test is slow, and the value of running it on every access method maybe somewhat
    # questionable, since the code for skipping over subtrees during RTS is not dependent on
    # access method. However, that relies on the aggregated timestamps in internal nodes
    # being correct, which _is_ dependent on access method, so running all three versions is
    # probably still worthwhile. However, if cutting back on test time becomes desirable it
    # is probably reasonable to run only one of these unless -l is given.
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
        config = 'cache_size=500MB,statistics=(all),verbose=(rts:5)'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000000

        # Create a table.
        uri = "table:rollback_to_stable12"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='split_pct=50')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)

        # Pin stable to timestamp 28 if prepare otherwise 20.
        # We prepare at commit_ts - 1 (so 29) and this is required to be strictly
        # greater than (not >=) stable.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(28))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Load a single row modification to be removed.
        commit_ts = 30
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[ds.key(1)] = value_b
        if self.prepare:
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
            self.session.commit_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

        self.session.checkpoint()

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, None, 30)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        pages_walk_skipped = stat_cursor[stat.conn.txn_rts_tree_walk_skip_pages][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreaterEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, 0)
        self.assertEqual(hs_sweep, 0)
        self.assertGreaterEqual(pages_walk_skipped, 0)

if __name__ == '__main__':
    wttest.run()
