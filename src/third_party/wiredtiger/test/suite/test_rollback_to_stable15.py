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

import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_rollback_to_stable15.py
# Test that roll back to stable handles updates present in the
# update-list for both fixed length and variable length column store.
# Eviction is set to false, so that everything persists in memory.
class test_rollback_to_stable15(wttest.WiredTigerTestCase):
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer-row', dict(key_format='i')),
    ]
    value_format_values = [
        # Fixed length
        ('fixed', dict(value_format='8t')),
        # Variable length
        ('variable', dict(value_format='i')),
    ]
    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]
    scenarios = make_scenarios(key_format_values, value_format_values, in_memory_values)

    def conn_config(self):
        config = 'cache_size=200MB,statistics=(all),debug_mode=(eviction=false)'
        if self.in_memory:
            config += ',in_memory=true'
        else:
            config += ',in_memory=false'
        return config

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        cursor.close()
        self.assertEqual(count, nrows)

    def test_rollback_to_stable(self):
        # Create a table.
        uri = "table:rollback_to_stable15"
        nrows = 2000
        create_params = 'log=(enabled=false),key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        value20 = 0x20
        value30 = 0x30
        value30 = 0x40
        value40 = 0x50

        #Insert value20 at timestamp 2
        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[i] = value20
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        #First Update to value 30 at timestamp 5
        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[i] = value30
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        cursor.close()

        #Set stable timestamp to 2
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.conn.rollback_to_stable()
        # Check that only value20 is available
        self.check(value20, uri, nrows - 1, 2)

        #Second Update to value30 at timestamp 7
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[i] = value30
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(7))

        #Third Update to value40 at timestamp 9
        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[i] = value40
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(9))
        cursor.close()

        #Set stable timestamp to 7
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(7))
        self.conn.rollback_to_stable()
        #Check that only value30 is available
        self.check(value30, uri, nrows - 1, 7)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()
        self.assertEqual(upd_aborted, (nrows*2) - 2)
        self.assertEqual(calls, 2)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
