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

import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_hs08.py
# Verify modify insert into history store logic.
class test_hs08(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=100MB,statistics=(all)'
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer-row', dict(key_format='i')),
    ]
    scenarios = make_scenarios(key_format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_modify_insert_to_hs(self):
        uri = "table:test_hs08"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'a' * 1000
        self.session.create(uri, create_params)

        # Insert a full value.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
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

        # Call checkpoint.
        self.session.checkpoint('use_timestamp=true')

        # Validate that we did write at least once to the history store.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        squashed_write = self.get_stat(stat.conn.cache_hs_write_squash)
        self.assertGreaterEqual(hs_writes, 1)
        self.assertEqual(squashed_write, 0)

        # Validate that we see the correct value at each of the timestamps.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(3))
        self.assertEqual(cursor[1], value1 + 'A')
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(4))
        self.assertEqual(cursor[1], value1 + 'AB')
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(5))
        self.assertEqual(cursor[1], value1 + 'ABC')
        self.session.commit_transaction()

        # Insert another two modifies. When we call checkpoint the first modify
        # will get written to the data store as a full value and the second will
        # be written to the data store as a reverse delta.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('D', 1000, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(7))

        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('E', 1001, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(8))

        # Call checkpoint again.
        self.session.checkpoint('use_timestamp=true')

        # Validate that we wrote to the history store again.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        squashed_write = self.get_stat(stat.conn.cache_hs_write_squash)
        self.assertGreaterEqual(hs_writes, 2)
        self.assertEqual(squashed_write, 0)

        # Validate that we see the expected value on the modifies, this
        # scenario tests the logic that will retrieve a full value for
        # a modify previously inserted into the history store.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(7))
        self.assertEqual(cursor[1], value1 + 'DBC')
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(8))
        self.assertEqual(cursor[1], value1 + 'DEC')
        self.session.commit_transaction()

        # Insert multiple modifies in the same transaction the first two should be squashed.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('F', 1002, 1)]), 0)
        self.assertEqual(cursor.modify([wiredtiger.Modify('G', 1003, 1)]), 0)
        self.assertEqual(cursor.modify([wiredtiger.Modify('H', 1004, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(9))

        # Call checkpoint again.
        self.session.checkpoint('use_timestamp=true')

        # Validate that we squashed two modifies. Note we can't count the exact number
        # we squashed, just that we did squash.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        squashed_write = self.get_stat(stat.conn.cache_hs_write_squash)
        self.assertGreaterEqual(hs_writes, 3)
        self.assertEqual(squashed_write, 1)

        # Insert multiple modifies in two different transactions so we should squash two.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('F', 1002, 1)]), 0)
        self.assertEqual(cursor.modify([wiredtiger.Modify('G', 1003, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(cursor.modify([wiredtiger.Modify('F', 1002, 1)]), 0)
        self.assertEqual(cursor.modify([wiredtiger.Modify('G', 1003, 1)]), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(11))

        # Call checkpoint again.
        self.session.checkpoint('use_timestamp=true')

        # Validate that we squashed two modifies. We also squashed a modify that was previously
        # squashed hence the number actually goes up by three.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        squashed_write = self.get_stat(stat.conn.cache_hs_write_squash)
        self.assertGreaterEqual(hs_writes, 4)
        self.assertEqual(squashed_write, 4)

        # Insert multiple modifies in different transactions with different timestamps on each
        # modify to guarantee we squash zero modifies.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(12))
        self.assertEqual(cursor.modify([wiredtiger.Modify('F', 1002, 1)]), 0)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(13))
        self.assertEqual(cursor.modify([wiredtiger.Modify('G', 1003, 1)]), 0)
        self.session.commit_transaction()

        self.session.begin_transaction()
        cursor.set_key(1)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(14))
        self.assertEqual(cursor.modify([wiredtiger.Modify('F', 1002, 1)]), 0)
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(15))
        self.assertEqual(cursor.modify([wiredtiger.Modify('G', 1003, 1)]), 0)
        self.session.commit_transaction()

        # Call checkpoint again.
        self.session.checkpoint('use_timestamp=true')

        # Validate that we squashed zero modifies.
        hs_writes = self.get_stat(stat.conn.cache_write_hs)
        squashed_write = self.get_stat(stat.conn.cache_hs_write_squash)
        self.assertGreaterEqual(hs_writes, 5)
        self.assertEqual(squashed_write, 5)

if __name__ == '__main__':
    wttest.run()
