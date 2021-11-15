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
# prepare
# [END_TAGS]

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_prepare09.py
# Validate scenarios involving inserting tombstones when rolling back prepares
class test_prepare09(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_prepared_update_is_aborted_correctly_with_on_disk_value(self):
        uri = "table:test_prepare09"
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
            value3 = 99
        else:
            value1 = 'a' * 10000
            value2 = 'b' * 10000
            value3 = 'c' * 10000

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
        ',stable_timestamp=' + self.timestamp_str(1))

        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)

        # Insert a new value.
        self.session.begin_transaction()
        cursor[1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Get the previous update onto disk.
        for i in range(2, 10000):
            cursor2[i] = value2
        cursor2.reset()

        # Prepare a full value and roll it back.
        self.session.begin_transaction()
        cursor[1] = value3
        self.assertEqual(self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3)), 0)
        self.session.rollback_transaction()

        # Get the previous update onto disk.
        for i in range(2, 10000):
            cursor2[i] = value3
        cursor2.reset()

        # Do a search, if we've aborted the update correct we won't have inserted a tombstone
        # and the original value will be visible to us.
        cursor.set_key(1)
        self.assertEquals(cursor.search(), 0)
        self.assertEquals(cursor.get_value(), value1)

    def test_prepared_update_is_aborted_correctly(self):
        uri = "table:test_prepare09"
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
            value3 = 99
            value4 = 100
        else:
            value1 = 'a' * 10000
            value2 = 'b' * 10000
            value3 = 'e' * 10000
            value4 = 'd' * 10000

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
        ',stable_timestamp=' + self.timestamp_str(1))

        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)

        # Prepare a full value.
        self.session.begin_transaction()
        cursor[1] = value1
        cursor[2] = value2
        cursor[3] = value3
        self.assertEqual(self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3)), 0)

        # Get the prepare onto disk.
        for i in range(4, 10000):
            cursor2[i] = value4
        cursor2.reset()

        # Rollback the prepare.
        self.assertEqual(self.session.rollback_transaction(), 0)

        # Search for key one, we should get not found.
        # (Except for FLCS, where for now at least the table extends itself under uncommitted
        # updates, so we expect to see 0.)
        cursor.set_key(1)
        if self.value_format == '8t':
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), 0)
        else:
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

if __name__ == '__main__':
    wttest.run()
