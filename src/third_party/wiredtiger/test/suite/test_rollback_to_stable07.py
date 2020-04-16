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

import fnmatch, os, shutil, time
from helper import copy_wiredtiger_home
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

# test_rollback_to_stable07.py
# Test the rollback to stable operation performs as expected following a server crash and
# recovery. Verify that
#   (a) the on-disk value is replaced by the correct value from the history store, and
#   (b) newer updates are removed.
class test_rollback_to_stable07(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(prepare_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),log=(enabled=true)'
        return config

    def simulate_crash_restart(self, uri, olddir, newdir):
        ''' Simulate a crash from olddir and restart in newdir. '''
        # with the connection still open, copy files to new directory
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        for fname in os.listdir(olddir):
            fullname = os.path.join(olddir, fname)
            # Skip lock file on Windows since it is locked
            if os.path.isfile(fullname) and \
                "WiredTiger.lock" not in fullname and \
                "Tmplog" not in fullname and \
                "Preplog" not in fullname:
                shutil.copy(fullname, newdir)
        #
        # close the original connection and open to new directory
        # NOTE:  This really cannot test the difference between the
        # write-no-sync (off) version of log_flush and the sync
        # version since we're not crashing the system itself.
        #
        self.close_conn()
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable07"
        ds = SimpleDataSet(
            self, uri, 0, key_format="i", value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        value_c = "ccccc" * 100
        value_d = "ddddd" * 100

        # Perform several updates.
        self.large_updates(uri, value_d, ds, nrows, 20)
        self.large_updates(uri, value_c, ds, nrows, 30)
        self.large_updates(uri, value_b, ds, nrows, 40)
        self.large_updates(uri, value_a, ds, nrows, 50)

        # Verify data is visible and correct.
        self.check(value_d, uri, nrows, 20)
        self.check(value_c, uri, nrows, 30)
        self.check(value_b, uri, nrows, 40)
        self.check(value_a, uri, nrows, 50)

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(40))

        # Perform additional updates.
        self.large_updates(uri, value_b, ds, nrows, 60)
        self.large_updates(uri, value_c, ds, nrows, 70)
        self.large_updates(uri, value_d, ds, nrows, 80)

        # Checkpoint to ensure the data is flushed to disk.
        self.session.checkpoint()

        # Verify additional update data is visible and correct.
        self.check(value_b, uri, nrows, 60)
        self.check(value_c, uri, nrows, 70)
        self.check(value_d, uri, nrows, 80)

        # Simulate a server crash and restart.
        self.simulate_crash_restart(uri, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_b, uri, nrows, 40)
        self.check(value_b, uri, nrows, 80)
        self.check(value_c, uri, nrows, 30)
        self.check(value_d, uri, nrows, 20)

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
        self.simulate_crash_restart(uri, "RESTART", "RESTART2")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_b, uri, nrows, 40)
        self.check(value_b, uri, nrows, 80)
        self.check(value_c, uri, nrows, 30)
        self.check(value_d, uri, nrows, 20)

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
