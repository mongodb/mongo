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

import fnmatch, os, shutil, threading, time
from wtthread import checkpoint_thread, op_thread
from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint_snapshot02.py
#   This test is to run checkpoint and eviction in parallel with timing
#   stress for checkpoint and let eviction write more data than checkpoint.
#

def timestamp_str(t):
    return '%x' % t
class test_checkpoint_snapshot02(wttest.WiredTigerTestCase):

    # Create a table.
    uri = "table:test_checkpoint_snapshot02"
    nrows = 1000

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true),timing_stress_for_test=[checkpoint_slow]'
        return config

    def large_updates(self, uri, value, ds, nrows):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def check(self, check_value, uri, nrows):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)

    def test_checkpoint_snapshot(self):

        ds = SimpleDataSet(self, self.uri, 0, key_format="S", value_format="S",config='log=(enabled=false)')
        ds.populate()
        valuea = "aaaaa" * 100
        valueb = "bbbbb" * 100
        valuec = "ccccc" * 100
        valued = "ddddd" * 100

        cursor = self.session.open_cursor(self.uri)
        self.large_updates(self.uri, valuea, ds, self.nrows)

        self.check(valuea, self.uri, self.nrows)

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)

        for i in range(self.nrows, self.nrows*2):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(valuea)
            self.assertEqual(cursor1.insert(), 0)

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()
            # Sleep for sometime so that checkpoint starts before committing last transaction.
            time.sleep(2)
            session1.commit_transaction()

        finally:
            done.set()
            ckpt.join()

        #Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the new directory.
        self.conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(self.conn)

        # Check the table contains the last checkpointed value.
        self.check(valuea, self.uri, self.nrows)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertEqual(upd_aborted, 0)
        self.assertGreaterEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreaterEqual(pages_visited, 0)

if __name__ == '__main__':
    wttest.run()
