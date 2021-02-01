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

import fnmatch, os, shutil, threading, time
from helper import copy_wiredtiger_home
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat, wiredtiger_strerror, WiredTigerError, WT_ROLLBACK
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread, op_thread
from time import sleep

def timestamp_str(t):
    return '%x' % t

def mod_val(value, char, location, nbytes=1):
    return value[0:location] + char + value[location+nbytes:]

def retry_rollback(self, name, txn_session, code):
    retry_limit = 100
    retries = 0
    completed = False
    saved_exception = None
    while not completed and retries < retry_limit:
        if retries != 0:
            self.pr("Retrying operation for " + name)
            if txn_session:
                txn_session.rollback_transaction()
            sleep(0.1)
            if txn_session:
                txn_session.begin_transaction('isolation=snapshot')
                self.pr("Began new transaction for " + name)
        try:
            code()
            completed = True
        except WiredTigerError as e:
            rollback_str = wiredtiger_strerror(WT_ROLLBACK)
            if rollback_str not in str(e):
                raise(e)
            retries += 1
            saved_exception = e
    if not completed and saved_exception:
        raise(saved_exception)

# test_rollback_to_stable14.py
# Test the rollback to stable operation uses proper base update while restoring modifies from history store.
class test_rollback_to_stable14(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(prepare_values)

    def conn_config(self):
        config = 'cache_size=8MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true),timing_stress_for_test=[history_store_checkpoint_delay]'
        return config

    def simulate_crash_restart(self, olddir, newdir):
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
        nrows = 1500

        # Create a table without logging.
        self.pr("create/populate table")
        uri = "table:rollback_to_stable14"
        ds = SimpleDataSet(
            self, uri, 0, key_format="i", value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = mod_val(value_a, 'Q', 0)
        value_modR = mod_val(value_modQ, 'R', 1)
        value_modS = mod_val(value_modR, 'S', 2)
        value_modT = mod_val(value_modS, 'T', 3)

        # Perform a combination of modifies and updates.
        self.pr("large updates and modifies")
        self.large_updates(uri, value_a, ds, nrows, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, 30)
        self.large_modifies(uri, 'R', ds, 1, 1, nrows, 40)
        self.large_modifies(uri, 'S', ds, 2, 1, nrows, 50)
        self.large_modifies(uri, 'T', ds, 3, 1, nrows, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, 20)
        self.check(value_modQ, uri, nrows, 30)
        self.check(value_modR, uri, nrows, 40)
        self.check(value_modS, uri, nrows, 50)
        self.check(value_modT, uri, nrows, 60)

        # Pin stable to timestamp 60 if prepare otherwise 50.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(60))
        else:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(50))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            self.pr("start checkpoint")
            ckpt.start()

            # Perform several modifies in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.pr("modifies")
            retry_rollback(self, 'modify ds1, W', None,
                           lambda: self.large_modifies(uri, 'W', ds, 4, 1, nrows, 70))
            retry_rollback(self, 'modify ds1, X', None,
                           lambda: self.large_modifies(uri, 'X', ds, 5, 1, nrows, 80))
            retry_rollback(self, 'modify ds1, Y', None,
                           lambda: self.large_modifies(uri, 'Y', ds, 6, 1, nrows, 90))
            retry_rollback(self, 'modify ds1, Z', None,
                           lambda: self.large_modifies(uri, 'Z', ds, 7, 1, nrows, 100))
        finally:
            done.set()
            ckpt.join()

        # Simulate a server crash and restart.
        self.pr("restart")
        self.simulate_crash_restart(".", "RESTART")
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
        self.assertEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows)
        self.assertGreaterEqual(hs_sweep, 0)

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, 20)
        self.check(value_modQ, uri, nrows, 30)
        self.check(value_modR, uri, nrows, 40)
        self.check(value_modS, uri, nrows, 50)

        # The test may output the following message in eviction under cache pressure. Ignore that.
        self.ignoreStdoutPatternIfExists("oldest pinned transaction ID rolled back for eviction")

    def test_rollback_to_stable_same_ts(self):
        nrows = 1500

        # Create a table without logging.
        self.pr("create/populate table")
        uri = "table:rollback_to_stable14"
        ds = SimpleDataSet(
            self, uri, 0, key_format="i", value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        value_a = "aaaaa" * 100

        value_modQ = mod_val(value_a, 'Q', 0)
        value_modR = mod_val(value_modQ, 'R', 1)
        value_modS = mod_val(value_modR, 'S', 2)
        value_modT = mod_val(value_modS, 'T', 3)

        # Perform a combination of modifies and updates.
        self.pr("large updates and modifies")
        self.large_updates(uri, value_a, ds, nrows, 20)
        self.large_modifies(uri, 'Q', ds, 0, 1, nrows, 30)
        # prepare cannot use same timestamp always, so use a different timestamps that are aborted.
        if self.prepare:
            self.large_modifies(uri, 'R', ds, 1, 1, nrows, 51)
            self.large_modifies(uri, 'S', ds, 2, 1, nrows, 55)
            self.large_modifies(uri, 'T', ds, 3, 1, nrows, 60)
        else:
            self.large_modifies(uri, 'R', ds, 1, 1, nrows, 60)
            self.large_modifies(uri, 'S', ds, 2, 1, nrows, 60)
            self.large_modifies(uri, 'T', ds, 3, 1, nrows, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, 20)
        self.check(value_modQ, uri, nrows, 30)
        self.check(value_modT, uri, nrows, 60)

        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(50))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            self.pr("start checkpoint")
            ckpt.start()

            # Perform several modifies in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.pr("modifies")
            retry_rollback(self, 'modify ds1, W', None,
                           lambda: self.large_modifies(uri, 'W', ds, 4, 1, nrows, 70))
            retry_rollback(self, 'modify ds1, X', None,
                           lambda: self.large_modifies(uri, 'X', ds, 5, 1, nrows, 80))
            retry_rollback(self, 'modify ds1, Y', None,
                           lambda: self.large_modifies(uri, 'Y', ds, 6, 1, nrows, 90))
            retry_rollback(self, 'modify ds1, Z', None,
                           lambda: self.large_modifies(uri, 'Z', ds, 7, 1, nrows, 100))
        finally:
            done.set()
            ckpt.join()

        # Simulate a server crash and restart.
        self.pr("restart")
        self.simulate_crash_restart(".", "RESTART")
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
        self.assertEqual(upd_aborted, 0)
        self.assertGreater(pages_visited, 0)
        self.assertGreaterEqual(hs_removed, nrows * 3)
        self.assertGreaterEqual(hs_sweep, 0)

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, 20)
        self.check(value_modQ, uri, nrows, 30)

        # The test may output the following message in eviction under cache pressure. Ignore that.
        self.ignoreStdoutPatternIfExists("oldest pinned transaction ID rolled back for eviction")

if __name__ == '__main__':
    wttest.run()
