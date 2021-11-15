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
from wiredtiger import stat, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread

# test_rollback_to_stable26.py
# Test the rollback to stable does properly restore the prepare rollback entry
# from the history store.
class test_rollback_to_stable26(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    hs_remove_values = [
        ('no_hs_remove', dict(hs_remove=False)),
        ('hs_remove', dict(hs_remove=True))
    ]

    prepare_remove_values = [
        ('no_prepare_remove', dict(prepare_remove=False)),
        ('prepare_remove', dict(prepare_remove=True))
    ]

    scenarios = make_scenarios(format_values, hs_remove_values, prepare_remove_values)

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),timing_stress_for_test=[history_store_checkpoint_delay]'
        return config

    def evict_cursor(self, uri, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (1, nrows + 1):
            evict_cursor.set_key(i)
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def test_rollback_to_stable(self):
        nrows = 10

        # Create a table without logging.
        uri = "table:rollback_to_stable26"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
             value_a = 97
             value_b = 98
             value_c = 99
             value_d = 100
             value_e = 101
        else:
             value_a = "aaaaa" * 100
             value_b = "bbbbb" * 100
             value_c = "ccccc" * 100
             value_d = "ddddd" * 100
             value_e = "eeeee" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        self.large_updates(uri, value_a, ds, nrows, False, 20)
        self.large_updates(uri, value_b, ds, nrows, False, 30)

        if self.hs_remove:
            self.large_removes(uri, ds, nrows, False, 40)

        prepare_session = self.conn.open_session()
        prepare_session.begin_transaction()
        cursor = prepare_session.open_cursor(uri)
        for i in range (1, nrows + 1):
            cursor[i] = value_c
            if self.prepare_remove:
                cursor.set_key(i)
                self.assertEqual(cursor.remove(), 0)
        cursor.close()
        prepare_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50))

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)

        self.evict_cursor(uri, nrows)

        # Pin stable to timestamp 40.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()
            # Sleep for sometime so that checkpoint starts before committing last transaction.
            time.sleep(5)
            prepare_session.rollback_transaction()
        finally:
            done.set()
            ckpt.join()

        self.large_updates(uri, value_d, ds, nrows, False, 60)

        # Check that the correct data.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_d, uri, nrows, None, 60)

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restore_updates = stat_cursor[stat.conn.txn_rts_hs_restore_updates][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertEqual(keys_removed, 0)
        self.assertEqual(hs_restore_updates, nrows)
        self.assertEqual(hs_removed, nrows)

        # Check that the correct data.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)

        self.large_updates(uri, value_e, ds, nrows, False, 60)

        self.evict_cursor(uri, nrows)

        # Check that the correct data.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_e, uri, nrows, None, 60)

if __name__ == '__main__':
    wttest.run()
