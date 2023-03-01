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
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread

# test_rollback_to_stable39.py
# Test to delay checkpoint and perform eviction in parallel to ensure eviction moves the content from data store to history store
# and then checkpoint history store to see the same content in data store and history store. Later use the checkpoint to restore
# the database which will trigger eviction to insert the same record from data store to history store.
class test_rollback_to_stable39(test_rollback_to_stable_base):
    restart_config = False

    format_values = [
        ('column', dict(key_format='r', value_format='S', prepare_extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            prepare_extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('row_integer', dict(key_format='i', value_format='S', prepare_extraconfig='')),
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=25MB,statistics=(all),statistics_log=(json,on_close,wait=1),verbose=(rts:5)'
        if self.restart_config:
            config += ',timing_stress_for_test=[checkpoint_slow]'
        else:
            config += ',timing_stress_for_test=[history_store_checkpoint_delay]'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table.
        uri = "table:rollback_to_stable39"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)

        self.large_removes(uri, ds, nrows, self.prepare, 30)
        # Verify no data is visible.
        self.check(value_a, uri, 0, nrows, 31 if self.prepare else 30)

        # Pin stable to timestamp 40 if prepare otherwise 30.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40 if self.prepare else 30))

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

            # Perform several updates in parallel with checkpoint.
            # Rollbacks may occur when checkpoint is running, so retry as needed.
            self.retry_rollback('update ds, e', None,
                           lambda: self.large_updates(uri, value_b, ds, nrows, self.prepare, 50))
            self.evict_cursor(uri, nrows, value_b)
        finally:
            done.set()
            ckpt.join()

        # Simulate a crash by copying to a new directory(RESTART).
        self.restart_config = True
        simulate_crash_restart(self, ".", "RESTART")
        
        # Check that the correct data is seen at and after the stable timestamp.
        self.check(value_a, uri, nrows, None, 21 if self.prepare else 20)
        self.check(value_a, uri, 0, nrows, 40)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_sweep = stat_cursor[stat.conn.txn_rts_sweep_hs_keys][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertEqual(upd_aborted, 0)
        self.assertEqual(hs_removed, 0)
        self.assertEqual(hs_sweep, 0)

        # Perform several updates.
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        self.check(value_c, uri, nrows, None, 61 if self.prepare else 60)

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

            self.evict_cursor(uri, nrows, value_c)
        finally:
            done.set()
            ckpt.join()


if __name__ == '__main__':
    wttest.run()

