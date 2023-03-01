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

from helper import simulate_crash_restart
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable40.py
# Test the rollback to stable operation performs as expected following a server crash
# and recovery. Verify that the on-disk value is replaced by the correct value from
# the history store.
class test_rollback_to_stable40(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)

    def conn_config(self):
        config = 'cache_size=1MB,statistics=(all),log=(enabled=true),verbose=(rts:5)'
        return config

    def test_rollback_to_stable(self):
        nrows = 3

        # Create a table without logging.
        uri = "table:rollback_to_stable40"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format="S", config='log=(enabled=false)')
        ds.populate()

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        value_c = "ccccc" * 100
        value_d = "ddddd" * 100

        # Insert 3 keys with same updates.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[1] = value_a
        cursor[2] = value_a
        cursor[3] = value_a
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        
        # Update the first and last key with another value with a large timestamp.
        self.session.begin_transaction()
        cursor[1] = value_d
        cursor[3] = value_d
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1000))

        # Update the middle key with lot of updates to generate more history.
        for i in range(21, 499):
            self.session.begin_transaction()
            cursor[2] = value_b + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))

        # With this checkpoint, all the updates in the history store are persisted to disk.
        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[2] = value_c
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(500))

        # Pin oldest and stable to timestamp 500.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(500) +
            ',stable_timestamp=' + self.timestamp_str(500))

        # Evict the globally visible update to write to the disk, this will reset the time window.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        evict_cursor.set_key(2)
        self.assertEqual(evict_cursor[2], value_c)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

        self.session.begin_transaction()
        cursor[2] = value_d
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(501))

        # 1. This checkpoint will move the globally visible update to the first of the key range.
        # 2. The existing updates in the history store are having with a larger timestamp are 
        #    obsolete, so they are not explicitly removed.
        # 3. Any of the history store updates that are already evicted will not rewrite by the
        #    checkpoint.
        self.session.checkpoint()

        # Verify data is visible and correct.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(1000))
        for i in range (1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)
            self.assertEquals(cursor.get_value(), value_d)
        self.session.rollback_transaction()
        cursor.close()

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Verify data is visible and correct.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(1000))
        for i in range (1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)
            if i % 2 == 0:
                self.assertEquals(cursor.get_value(), value_c)
            else:
                self.assertEquals(cursor.get_value(), value_a)
        self.session.rollback_transaction()

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
        self.assertGreaterEqual(hs_removed, 3)

if __name__ == '__main__':
    wttest.run()
