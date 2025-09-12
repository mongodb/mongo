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

from helper import copy_wiredtiger_home
from suite_subprocess import suite_subprocess
import wiredtiger, wttest, time, threading
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtthread import checkpoint_thread

# test_hs33.py
# Test that we can run recovery after crashing before the metadata is synced during recovery.
# This simulates the scenario seen in WT-14376, where the eviction subsystem opened the history
# store file before metadata recovery had completed.
class test_hs33(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'statistics=(all)'

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, session, uri, value, ds, nrows, timestamp=False):
        cursor = session.open_cursor(uri)
        for i in range(1, nrows):
            if timestamp == True:
                session.begin_transaction()
            cursor.set_key(ds.key(i))
            cursor.set_value(value)
            self.assertEqual(cursor.update(), 0)
            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))
        cursor.close()

    def add_insert(self, uri, ds, value, nrows):
        session = self.session
        cursor = session.open_cursor(uri)
        self.pr('insert: ' + uri + ' for ' + str(nrows) + ' rows')
        for i in range(1, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def test_hs_recovery(self):
        ntables = 100
        nrows = 5

        bigvalue = b"aaaaa" * 100
        bigvalue2 = b"ccccc" * 100

        # Create a large number of tables.
        tables = {}
        for i in range(1, ntables):
            uri = 'table:table' + str(i)
            ds = SimpleDataSet(self, uri, 0, key_format='S', value_format='u',
                               config='log=(enabled=false)')
            ds.populate()
            tables[uri] = ds

        # Insert an initial set of data.
        for uri, ds in tables.items():
            self.add_insert(uri, ds, bigvalue, nrows)

        # Apply some updates to move data to the history store.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        # Large updates with session 1.
        for uri, ds in tables.items():
            self.large_updates(self.session, uri, bigvalue2, ds, nrows)

        # Wait until we reach the checkpoint stop timing stress point before copying the
        # database. This point is placed before we sync the metadata file so that the snapshot
        # includes incomplete checkpoint log records.
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_stop]')

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Poll the checkpoint stop timing stress stat until we know we've reached the stress
            # point.
            ckpt_stop_timing_stress = 0
            while not ckpt_stop_timing_stress:
                time.sleep(1)
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_stop_timing_stress = stat_cursor[stat.conn.checkpoint_stop_stress_active][2]
                stat_cursor.close()

            copy_wiredtiger_home(self, '.', "RESTART")

        finally:
            done.set()
            ckpt.join()

        session2.rollback_transaction()
        session2.close()

        # Open the new directory, triggering recovery. Set a low eviction size and low eviction
        # triggers to trigger the eviction checks during metadata log replay. Prior to the fix in
        # WT-14391, this would cause the history store to open during the metadata recovery which
        # should not occur. Files should only be opened after metadata recovery to ensure the
        # correct checkpoint is loaded.
        self.close_conn()
        self.conn_config = 'cache_size=1MB,eviction_dirty_trigger=2,eviction_dirty_target=1,statistics=(all)'
        conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(conn)
