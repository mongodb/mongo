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
from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread

def mod_val(value, char, location, nbytes=1):
    return value[0:location] + char + value[location+nbytes:]

def append_val(value, char):
    return value + char

# test_rollback_to_stable14.py
# Test the rollback to stable operation uses proper base update while restoring modifies from
# history store. Since FLCS inherently doesn't support modify, there's no need to run this on
# FLCS. (Note that self.value_format needs to exist anyway for the base class to use.)
class test_rollback_to_stable14(test_rollback_to_stable_base):

    key_format_values = [
        ('column', dict(key_format='r')),
        ('row_integer', dict(key_format='i')),
    ]
    value_format='S'

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(key_format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=25MB,statistics=(all),statistics_log=(json,on_close,wait=1),timing_stress_for_test=[history_store_checkpoint_delay]'
        return config

    def test_rollback_to_stable(self):
        nrows = 100

        # Create a table.
        self.pr("create/populate table")
        uri = "table:rollback_to_stable14"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = mod_val(value_a, 'Q', 0)
        value_modR = mod_val(value_modQ, 'R', 1)
        value_modS = mod_val(value_modR, 'S', 2)
        value_modT = mod_val(value_modS, 'T', 3)
        value_modW = mod_val(value_modT, 'W', 4)
        value_modX = mod_val(value_modW, 'X', 5)
        value_modY = mod_val(value_modX, 'Y', 6)
        value_modZ = mod_val(value_modY, 'Z', 7)

        # Perform a combination of modifies and updates.
        self.pr("large updates and modifies")
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, self.prepare, 30)
        self.large_modifies(uri, 'R', ds, 1, 1, nrows, self.prepare, 40)
        self.large_modifies(uri, 'S', ds, 2, 1, nrows, self.prepare, 50)
        self.large_modifies(uri, 'T', ds, 3, 1, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)
        self.check(value_modQ, uri, nrows, None, 31 if self.prepare else 30)
        self.check(value_modR, uri, nrows, None, 41 if self.prepare else 40)
        self.check(value_modS, uri, nrows, None, 51 if self.prepare else 50)
        self.check(value_modT, uri, nrows, None, 61 if self.prepare else 60)

        # Pin stable to timestamp 60 if prepare otherwise 50.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(60))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            self.pr("start checkpoint")
            ckpt.start()
            # Sleep for sometime so that checkpoint starts.
            time.sleep(2)

            # Perform several modifies in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.pr("modifies")
            self.retry_rollback('modify ds1, W', None,
                           lambda: self.large_modifies(uri, 'W', ds, 4, 1, nrows, self.prepare, 70))
            self.evict_cursor(uri, nrows, value_modW)
            self.retry_rollback('modify ds1, X', None,
                           lambda: self.large_modifies(uri, 'X', ds, 5, 1, nrows, self.prepare, 80))
            self.evict_cursor(uri, nrows, value_modX)
            self.retry_rollback('modify ds1, Y', None,
                           lambda: self.large_modifies(uri, 'Y', ds, 6, 1, nrows, self.prepare, 90))
            self.evict_cursor(uri, nrows, value_modY)
            self.retry_rollback('modify ds1, Z', None,
                           lambda: self.large_modifies(uri, 'Z', ds, 7, 1, nrows, self.prepare, 100))
            self.evict_cursor(uri, nrows, value_modZ)
        finally:
            done.set()
            ckpt.join()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        hs_restore_updates = stat_cursor[stat.conn.txn_rts_hs_restore_updates][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(hs_restore_updates, nrows)
        self.assertEqual(keys_restored, 0)
        if self.prepare:
            self.assertGreaterEqual(upd_aborted, 0)
        else:
            self.assertEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows)
        self.assertGreaterEqual(hs_sweep, 0)

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_modQ, uri, nrows, None, 30)
        self.check(value_modR, uri, nrows, None, 40)
        self.check(value_modS, uri, nrows, None, 50)

        # The test may output the following message in eviction under cache pressure. Ignore that.
        self.ignoreStdoutPatternIfExists("oldest pinned transaction ID rolled back for eviction")

    def test_rollback_to_stable_same_ts(self):
        nrows = 100

        # Create a table.
        self.pr("create/populate table")
        uri = "table:rollback_to_stable14"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = mod_val(value_a, 'Q', 0)
        value_modR = mod_val(value_modQ, 'R', 1)
        value_modS = mod_val(value_modR, 'S', 2)
        value_modT = mod_val(value_modS, 'T', 3)
        value_modW = mod_val(value_modT, 'W', 4)
        value_modX = mod_val(value_modW, 'X', 5)
        value_modY = mod_val(value_modX, 'Y', 6)
        value_modZ = mod_val(value_modY, 'Z', 7)

        # Perform a combination of modifies and updates.
        self.pr("large updates and modifies")
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, self.prepare, 30)
        # prepare cannot use same timestamp always, so use a different timestamps that are aborted.
        if self.prepare:
            self.large_modifies(uri, 'R', ds, 1, 1, nrows, self.prepare, 51)
            self.large_modifies(uri, 'S', ds, 2, 1, nrows, self.prepare, 55)
            self.large_modifies(uri, 'T', ds, 3, 1, nrows, self.prepare, 60)
        else:
            self.large_modifies(uri, 'R', ds, 1, 1, nrows, self.prepare, 60)
            self.large_modifies(uri, 'S', ds, 2, 1, nrows, self.prepare, 60)
            self.large_modifies(uri, 'T', ds, 3, 1, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)
        self.check(value_modQ, uri, nrows, None, 31 if self.prepare else 30)
        self.check(value_modT, uri, nrows, None, 61 if self.prepare else 60)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            self.pr("start checkpoint")
            ckpt.start()
            # Sleep for sometime so that checkpoint starts.
            time.sleep(2)

            # Perform several modifies in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.pr("modifies")
            self.retry_rollback('modify ds1, W', None,
                           lambda: self.large_modifies(uri, 'W', ds, 4, 1, nrows, self.prepare, 70))
            self.evict_cursor(uri, nrows, value_modW)
            self.retry_rollback('modify ds1, X', None,
                           lambda: self.large_modifies(uri, 'X', ds, 5, 1, nrows, self.prepare, 80))
            self.evict_cursor(uri, nrows, value_modX)
            self.retry_rollback('modify ds1, Y', None,
                           lambda: self.large_modifies(uri, 'Y', ds, 6, 1, nrows, self.prepare, 90))
            self.evict_cursor(uri, nrows, value_modY)
            self.retry_rollback('modify ds1, Z', None,
                           lambda: self.large_modifies(uri, 'Z', ds, 7, 1, nrows, self.prepare, 100))
            self.evict_cursor(uri, nrows, value_modZ)
        finally:
            done.set()
            ckpt.join()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restore_updates = stat_cursor[stat.conn.txn_rts_hs_restore_updates][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(hs_restore_updates, nrows)
        self.assertEqual(keys_restored, 0)
        if self.prepare:
            self.assertGreaterEqual(upd_aborted, 0)
        else:
            self.assertEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows * 3)
        self.assertGreaterEqual(hs_sweep, 0)

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_modQ, uri, nrows, None, 30)

        # The test may output the following message in eviction under cache pressure. Ignore that.
        self.ignoreStdoutPatternIfExists("oldest pinned transaction ID rolled back for eviction")

    def test_rollback_to_stable_same_ts_append(self):
        nrows = 100

        # Create a table.
        self.pr("create/populate table")
        uri = "table:rollback_to_stable14"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = append_val(value_a, 'Q')
        value_modR = append_val(value_modQ, 'R')
        value_modS = append_val(value_modR, 'S')
        value_modT = append_val(value_modS, 'T')
        value_modW = append_val(value_modT, 'W')
        value_modX = append_val(value_modW, 'X')
        value_modY = append_val(value_modX, 'Y')
        value_modZ = append_val(value_modY, 'Z')

        # Perform a combination of modifies and updates.
        self.pr("large updates and modifies")
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        self.large_modifies(uri, 'Q', ds, len(value_a), 1, nrows, self.prepare, 30)
        # prepare cannot use same timestamp always, so use a different timestamps that are aborted.
        if self.prepare:
            self.large_modifies(uri, 'R', ds, len(value_modQ), 1, nrows, self.prepare, 51)
            self.large_modifies(uri, 'S', ds, len(value_modR), 1, nrows, self.prepare, 55)
            self.large_modifies(uri, 'T', ds, len(value_modS), 1, nrows, self.prepare, 60)
        else:
            self.large_modifies(uri, 'R', ds, len(value_modQ), 1, nrows, self.prepare, 60)
            self.large_modifies(uri, 'S', ds, len(value_modR), 1, nrows, self.prepare, 60)
            self.large_modifies(uri, 'T', ds, len(value_modS), 1, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)
        self.check(value_modQ, uri, nrows, None, 31 if self.prepare else 30)
        self.check(value_modT, uri, nrows, None, 61 if self.prepare else 60)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            self.pr("start checkpoint")
            ckpt.start()
            # Sleep for sometime so that checkpoint starts.
            time.sleep(2)

            # Perform several modifies in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.pr("modifies")
            self.retry_rollback('modify ds1, W', None,
                           lambda: self.large_modifies(uri, 'W', ds, len(value_modT), 1, nrows, self.prepare, 70))
            self.retry_rollback('modify ds1, X', None,
                           lambda: self.large_modifies(uri, 'X', ds, len(value_modW), 1, nrows, self.prepare, 80))
            self.evict_cursor(uri, nrows, value_modX)
            self.retry_rollback('modify ds1, Y', None,
                           lambda: self.large_modifies(uri, 'Y', ds, len(value_modX), 1, nrows, self.prepare, 90))
            self.retry_rollback('modify ds1, Z', None,
                           lambda: self.large_modifies(uri, 'Z', ds, len(value_modY), 1, nrows, self.prepare, 100))
            self.evict_cursor(uri, nrows, value_modZ)
        finally:
            done.set()
            ckpt.join()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restore_updates = stat_cursor[stat.conn.txn_rts_hs_restore_updates][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(hs_restore_updates, nrows)
        self.assertEqual(keys_restored, 0)
        if self.prepare:
            self.assertGreaterEqual(upd_aborted, 0)
        else:
            self.assertEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows * 3)
        self.assertGreaterEqual(hs_sweep, 0)

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_modQ, uri, nrows, None, 30)

        # The test may output the following message in eviction under cache pressure. Ignore that.
        self.ignoreStdoutPatternIfExists("oldest pinned transaction ID rolled back for eviction")

if __name__ == '__main__':
    wttest.run()
