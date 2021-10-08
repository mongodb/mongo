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
#
# [TEST_TAGS]
# rollback_to_stable:prepare
# rollback_to_stable:out_of_order_timestamps
# [END_TAGS]

from wiredtiger import stat, WT_NOTFOUND
from wtscenario import make_scenarios
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable21.py
# Test rollback to stable when an out of order prepared transaction is written to disk
class test_rollback_to_stable21(test_rollback_to_stable_base):
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)

    def conn_config(self):
        config = 'cache_size=250MB,statistics=(all),statistics_log=(json,on_close,wait=1)'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable21"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        valuea = 'a' * 400
        valueb = 'b' * 400

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valuea

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valueb

        cursor.reset()
        cursor.close()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        s = self.conn.open_session()
        s.begin_transaction('ignore_prepare = true')
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")

        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEqual(evict_cursor.get_value(), valuea)
            evict_cursor.reset()

        s.rollback_transaction()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        s.checkpoint()

        # Rollback the prepared transaction
        self.session.rollback_transaction()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        self.check(valuea, uri, nrows, 40)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        stat_cursor.close()

        self.assertGreater(hs_removed, 0)

    def test_rollback_to_stable_with_different_tombstone(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable21"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        valuea = 'a' * 400
        valueb = 'b' * 400

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valuea
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor.set_key(i)
            cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valueb

        cursor.reset()
        cursor.close()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        s = self.conn.open_session()
        s.begin_transaction('ignore_prepare = true, read_timestamp = ' + self.timestamp_str(30))
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")

        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEqual(evict_cursor.get_value(), valuea)
            evict_cursor.reset()

        s.rollback_transaction()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        s.checkpoint()

        # Rollback the prepared transaction
        self.session.rollback_transaction()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        self.check(valuea, uri, nrows, 30)
        self.check(valuea, uri, 0, 40)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restored_tombstone = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        stat_cursor.close()

        self.assertGreater(hs_removed, 0)
        self.assertGreater(hs_restored_tombstone, 0)

    def test_rollback_to_stable_with_same_tombstone(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable21"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        valuea = 'a' * 400
        valueb = 'b' * 400

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valuea
            cursor.set_key(i)
            cursor.remove()

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[i] = valueb

        cursor.reset()
        cursor.close()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        s = self.conn.open_session()
        s.begin_transaction('ignore_prepare = true')
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")

        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
            evict_cursor.reset()

        s.rollback_transaction()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        s.checkpoint()

        # Rollback the prepared transaction
        self.session.rollback_transaction()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        self.check(valuea, uri, 0, 40)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        hs_restored_tombstone = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        stat_cursor.close()

        self.assertGreater(hs_removed, 0)
        self.assertGreater(hs_restored_tombstone, 0)
