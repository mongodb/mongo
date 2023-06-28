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

import wttest, threading, time
from wtdataset import SimpleDataSet
from wtthread import checkpoint_thread
from wiredtiger import stat
from helper import copy_wiredtiger_home
from wtscenario import make_scenarios
from rollback_to_stable_util import test_rollback_to_stable_base

# test_rollback_to_stable35.py
# Test that log is flushed for all writes that occurred in the checkpoint.
class test_rollback_to_stable35(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def large_updates(self, uri_1, uri_2, value, ds_1, ds_2, nrows):
        # Update a large number of records.
        session = self.session
        cursor_1 = session.open_cursor(uri_1)
        cursor_2 = session.open_cursor(uri_2)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor_1[ds_1.key(i)] = value
            cursor_2[ds_2.key(i)] = value
            session.commit_transaction()
        cursor_1.close()
        cursor_2.close()

    def check(self, check_value, uri_1, uri_2, nrows):
        session = self.session
        session.begin_transaction()
        cursor_1 = session.open_cursor(uri_1)
        cursor_2 = session.open_cursor(uri_2)
        count = 0
        for k, v in cursor_1:
            self.assertEqual(v, check_value)
            count += 1
        self.assertEqual(count, nrows)
        count = 0
        for k, v in cursor_2:
            self.assertEqual(v, check_value)
            count += 1
        self.assertEqual(count, nrows)
        session.commit_transaction()
        cursor_1.close()
        cursor_2.close()

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),log=(enabled,force_write_wait=60),timing_stress_for_test=[checkpoint_slow, checkpoint_stop],verbose=(rts:5)'
        return config

    def test_rollback_to_stable(self):
        nrows = 10

        # Create two tables.
        uri_1 = "table:rollback_to_stable35_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format)
        ds_1.populate()

        uri_2 = "table:rollback_to_stable35_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format)
        ds_2.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
            valuec = 99
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100
            valuec = "ccccc" * 100

        self.large_updates(uri_1, uri_2, valuea, ds_1, ds_2, nrows)
        self.check(valuea, uri_1, uri_2, nrows)

        # Start a long running transaction and keep it open.
        session_2 = self.conn.open_session()
        session_2.begin_transaction()

        self.large_updates(uri_1, uri_2, valueb, ds_1, ds_2, nrows)
        self.check(valueb, uri_1, uri_2, nrows)

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()
            # Wait for checkpoint to start before committing.
            ckpt_started = 0
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.txn_checkpoint_running][2]
                stat_cursor.close()
                time.sleep(1)
            
            self.large_updates(uri_1, uri_2, valuec, ds_1, ds_2, nrows)
            self.check(valuec, uri_1, uri_2, nrows)

            # Evict the data.
            self.evict_cursor(uri_1, nrows, valuec)

            # Wait for checkpoint stop timing stress to copy the database.
            ckpt_stop_timing_stress = 0
            while not ckpt_stop_timing_stress:
                time.sleep(1)
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_stop_timing_stress = stat_cursor[stat.conn.txn_checkpoint_stop_stress_active][2]
                stat_cursor.close()

            copy_wiredtiger_home(self, '.', "RESTART")

        finally:
            done.set()
            ckpt.join()
        self.session.checkpoint()

        # Clear all running transactions before rollback to stable.
        session_2.commit_transaction()
        session_2.close()

        # Open the new directory
        self.close_conn()
        self.conn_config = 'cache_size=50MB,statistics=(all),log=(enabled)'
        conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(conn)

        self.check(valuec, uri_1, uri_2, nrows)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        stat_cursor.close()

        self.assertEqual(calls, 0)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertEqual(pages_visited, 0)
        self.assertEqual(upd_aborted, 0)
        self.assertGreaterEqual(hs_removed, 0)

if __name__ == '__main__':
    wttest.run()
