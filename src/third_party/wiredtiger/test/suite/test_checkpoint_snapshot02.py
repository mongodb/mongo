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
from helper import simulate_crash_restart
import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint_snapshot02.py
#   This test is to run checkpoint and eviction in parallel with timing
#   stress for checkpoint and let eviction write more data than checkpoint.
#
class test_checkpoint_snapshot02(wttest.WiredTigerTestCase):

    # Create a table.
    uri = "table:test_checkpoint_snapshot02"

    format_values = [
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true),timing_stress_for_test=[checkpoint_slow]'
        return config

    def moresetup(self):
        if self.value_format == '8t':
            # Rig to use more than one page; otherwise the inconsistent checkpoint assertions fail.
            self.extraconfig = ',leaf_page_max=4096'
            self.nrows = 5000
            self.valuea = 97
        else:
            self.extraconfig = ''
            self.nrows = 1000
            self.valuea = "aaaaa" * 100

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows+1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            if commit_ts == 0:
                session.commit_transaction()
            else:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts, more_invisible_rows_exist):
        # In FLCS the existence of the invisible extra set of rows causes the table to
        # extend under them. Until that's fixed, expect (not just allow) those rows to
        # exist and demand that they read back as zero and not as check_value. When it
        # is fixed (so the end of the table updates transactionally) the special-case
        # logic can just be removed.
        flcs_tolerance = more_invisible_rows_exist and self.value_format == '8t'

        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if flcs_tolerance and count >= nrows:
                self.assertEqual(v, 0)
            else:
                self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows * 2 if flcs_tolerance else nrows)

    def test_checkpoint_snapshot(self):
        self.moresetup()

        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config='log=(enabled=false)'+self.extraconfig)
        ds.populate()

        self.large_updates(self.uri, self.valuea, ds, self.nrows, 0)
        self.check(self.valuea, self.uri, self.nrows, 0, False)

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)

        for i in range(self.nrows+1, (self.nrows*2)+1):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(self.valuea)
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
        simulate_crash_restart(self, ".", "RESTART")

        # Check the table contains the last checkpointed value.
        self.check(self.valuea, self.uri, self.nrows, 0, True)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertGreaterEqual(keys_removed, 0)

    def test_checkpoint_snapshot_with_timestamp(self):
        self.moresetup()

        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config='log=(enabled=false)'+self.extraconfig)
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        self.large_updates(self.uri, self.valuea, ds, self.nrows, 20)
        self.check(self.valuea, self.uri, self.nrows, 20, False)

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)

        for i in range(self.nrows+1, (self.nrows*2)+1):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(self.valuea)
            self.assertEqual(cursor1.insert(), 0)
        session1.timestamp_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Set stable timestamp to 40
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

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
        simulate_crash_restart(self, ".", "RESTART")

        # Check the table contains the last checkpointed value.
        self.check(self.valuea, self.uri, self.nrows, 30, True)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertGreaterEqual(keys_removed, 0)

    def test_checkpoint_snapshot_with_txnid_and_timestamp(self):
        self.moresetup()

        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config='log=(enabled=false)'+self.extraconfig)
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        session1 = self.conn.open_session()
        session1.begin_transaction()

        self.large_updates(self.uri, self.valuea, ds, self.nrows, 20)
        self.check(self.valuea, self.uri, self.nrows, 20, False)

        session2 = self.conn.open_session()
        session2.begin_transaction()
        cursor2 = session2.open_cursor(self.uri)

        for i in range((self.nrows+1), (self.nrows*2)+1):
            cursor2.set_key(ds.key(i))
            cursor2.set_value(self.valuea)
            self.assertEqual(cursor2.insert(), 0)
        session1.timestamp_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Set stable timestamp to 40
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()
            # Sleep for sometime so that checkpoint starts before committing last transaction.
            time.sleep(2)
            session2.commit_transaction()

        finally:
            done.set()
            ckpt.join()

        session1.rollback_transaction()
        #Simulate a crash by copying to a new directory(RESTART).
        simulate_crash_restart(self, ".", "RESTART")

        # Check the table contains the last checkpointed value.
        self.check(self.valuea, self.uri, self.nrows, 30, True)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertGreaterEqual(keys_removed, 0)

        simulate_crash_restart(self, "RESTART", "RESTART2")
        # Check the table contains the last checkpointed value.
        self.check(self.valuea, self.uri, self.nrows, 30, True)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreaterEqual(inconsistent_ckpt, 0)
        self.assertEqual(keys_removed, 0)

if __name__ == '__main__':
    wttest.run()
