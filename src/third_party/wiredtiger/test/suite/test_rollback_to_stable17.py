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
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_rollback_to_stable17.py
# Test that rollback to stable handles updates present on history store and data store.
class test_rollback_to_stable17(wttest.WiredTigerTestCase):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    scenarios = make_scenarios(format_values, in_memory_values)

    def conn_config(self):
        config = 'cache_size=200MB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def insert_update_data(self, uri, value, start_row, end_row, timestamp):
        cursor =  self.session.open_cursor(uri)
        for i in range(start_row, end_row):
            self.session.begin_transaction()
            cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)

        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1

        session.commit_transaction()
        self.assertEqual(count, nrows)
        cursor.close()

    def test_rollback_to_stable(self):
        # Create a table.
        uri = "table:rollback_to_stable17"
        nrows = 200
        start_row = 1
        ts = [2,5,7,9]
        if self.value_format == '8t':
            values = [97, 98, 99, 100]
        else:
            values = ["aaaa", "bbbb", "cccc", "dddd"]

        # Create a table.
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, config)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Make a series of updates for the same keys with different values at different timestamps.
        for i in range(len(values)):
            self.insert_update_data(uri, values[i], start_row, nrows, ts[i])

        # Set the stable timestamp to 5.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        if not self.in_memory:
            # Checkpoint to ensure that all the updates are flushed to disk.
            self.session.checkpoint()
            # Rollback to stable done as part of recovery.
            simulate_crash_restart(self,".", "RESTART")
        else:
            # Manually call rollback_to_stable for in memory keys/values.
            self.conn.rollback_to_stable()

        # Check that keys at timestamps 2 and 5 have the correct values they were updated with.
        self.check(values[0], uri, nrows - 1, 2)
        self.check(values[1], uri, nrows - 1, 5)
        # Check that the keys at timestamps 7 and 9 were rolled back to contain the value at the
        # stable timestamp 5.
        self.check(values[1], uri, nrows - 1, 7)
        self.check(values[1], uri, nrows - 1, 9)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        hs_removed = stat_cursor[stat.conn.txn_rts_hs_removed][2]
        stat_cursor.close()

        self.assertGreaterEqual(upd_aborted + hs_removed, (nrows*2) - 2)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
