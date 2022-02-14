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
# rollback_to_stable
# [END_TAGS]

from helper import simulate_crash_restart
import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_rollback_to_stable16.py
# Test that rollback to stable removes updates present on disk for column store.
# (This test is now probably redundant with others, and could maybe be removed?)
class test_rollback_to_stable16(wttest.WiredTigerTestCase):

    key_format_values = [
        ('column', dict(key_format='r')),
        ('row_integer', dict(key_format='i')),
    ]

    value_format_values = [
        # Fixed length
        ('fixed', dict(value_format='8t')),
        # Variable length
        ('variable', dict(value_format='S')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    def keep(name, d):
        if d['key_format'] == 'i' and d['value_format'] == '8t':
            # Fixed-length format is only special for column-stores.
            return False
        return True

    scenarios = make_scenarios(key_format_values, value_format_values, in_memory_values,
        include=keep)

    def conn_config(self):
        config = 'cache_size=200MB,statistics=(all)'
        if self.in_memory:
            config += ',in_memory=true'
        else:
            config += ',in_memory=false'
        return config

    def insert_update_data(self, uri, value, start_row, nrows, timestamp):
        cursor =  self.session.open_cursor(uri)
        for i in range(start_row, start_row + nrows):
            self.session.begin_transaction()
            if self.value_format == 'S':
                cursor[i] = value + str(i)
            else:
                cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))
        cursor.close()

    def check(self, check_value, uri, nrows, start_row, read_ts):
        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)

        count = 0
        for i in range(start_row, start_row + nrows):
            cursor.set_key(i)
            ret = cursor.search()
            if check_value is None:
                if ret != wiredtiger.WT_NOTFOUND:
                    self.assertTrue(ret == wiredtiger.WT_NOTFOUND)
            else:
                if self.value_format == 'S':
                    self.assertEqual(cursor.get_value(), check_value + str(count + start_row))
                else:
                    self.assertEqual(cursor.get_value(), check_value)
                count += 1

        session.commit_transaction()
        if check_value is None:
            self.assertEqual(count, 0)
        else:
            self.assertEqual(count, nrows)
        cursor.close()

    def test_rollback_to_stable16(self):
        # Create a table.
        uri = "table:rollback_to_stable16"
        nrows = 200
        start_row = 1
        ts = [2,5,7,9]
        if self.value_format == 'S':
            values = ["aaaa", "bbbb", "cccc", "dddd"]
        else:
            values = [0x01, 0x02, 0x03, 0x04]

        # Create a table.
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        for i in range(len(values)):
            self.insert_update_data(uri, values[i], start_row, nrows, ts[i])
            start_row += nrows

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        if not self.in_memory:
            # Checkpoint to ensure that all the updates are flushed to disk.
            self.session.checkpoint()
            # Rollback to stable done as part of recovery.
            simulate_crash_restart(self,".", "RESTART")
        else:
            # Manually call rollback_to_stable for in memory keys/values.
            self.conn.rollback_to_stable()

        self.check(values[0], uri, nrows, 1, 2)
        self.check(values[1], uri, nrows, 201, 5)
        self.check(0 if self.value_format == '8t' else None, uri, nrows, 401, 7)
        self.check(0 if self.value_format == '8t' else None, uri, nrows, 601, 9)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreaterEqual(upd_aborted + keys_removed, (nrows*2) - 2)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
