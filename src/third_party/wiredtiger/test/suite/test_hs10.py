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

import wiredtiger, wttest, time
from wiredtiger import stat
from wtscenario import make_scenarios

# test_hs10.py
# Verify modify read after eviction.
class test_hs10(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB,statistics=(all),eviction=(threads_max=1)'
    key_format_values = (
        ('column', dict(key_format='r')),
        ('integer-row', dict(key_format='i'))
    )
    scenarios = make_scenarios(key_format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_modify_insert_to_hs(self):
        uri = "table:test_hs10"
        uri2 = "table:test_hs10_otherdata"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'a' * 1000
        value2 = 'b' * 1000
        self.session.create(uri, create_params)
        session2 = self.setUpSessionOpen(self.conn)
        session2.create(uri2, create_params)
        cursor2 = session2.open_cursor(uri2)
        # Insert a full value.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Insert 3 modifies in separate transactions.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('A', 1000, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('B', 1001, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('C', 1002, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        # Insert a whole bunch of data into the other table to force wiredtiger to evict data
        # from the previous table.
        for i in range(1, 10000):
            cursor2[i] = value2

        # Validate that we see the correct value at each of the timestamps.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(3))
        cursor.set_key(1)
        cursor.search()
        self.assertEqual(cursor[1], value1 + 'A')
        self.session.commit_transaction()

        cursor2 = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(4))
        cursor2.set_key(1)
        cursor2.search()
        self.assertEqual(cursor2.get_value(), value1 + 'AB')
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(5))
        self.assertEqual(cursor[1], value1 + 'ABC')
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
