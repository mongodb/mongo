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

# test_rollback_to_stable13.py
# Test the rollback to stable should roll back the tombstone in the history store.
class test_rollback_to_stable13(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(prepare_values)

    def conn_config(self):
        config = 'cache_size=500MB,statistics=(all),log=(enabled=true)'
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
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable13"
        ds = SimpleDataSet(
            self, uri, 0, key_format="i", value_format="S", config='split_pct=50,log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, 20)

        # Perform several removes.
        self.large_removes(uri, ds, nrows, 30)

        # Perform several updates.
        self.large_updates(uri, value_b, ds, nrows, 60)

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, 20)
        self.check(None, uri, 0, 30)
        self.check(value_b, uri, nrows, 60)

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + timestamp_str(40))

        self.session.checkpoint()

        # Simulate a server crash and restart.
        self.simulate_crash_restart(".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(None, uri, 0, 50)

        # Check that we restore the correct value from the history store.
        self.check(value_a, uri, nrows, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        restored_tombstones = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        self.assertEqual(restored_tombstones, nrows)
