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
from wtscenario import make_scenarios

# test_timestamp20.py
# Exercise fixing up of out-of-order updates in the history store.
class test_timestamp20(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'

    format_values = [
        ('string-row', dict(key_format='S', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def get_key(self, i):
        return str(i) if self.key_format == 'S' else i

    def test_timestamp20_standard(self):
        uri = 'table:test_timestamp20'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        if self.value_format == '8t':
            value1 = 97 # 'a'
            value2 = 98 # 'b'
            value3 = 99 # 'c'
            value4 = 100 # 'd'
            value5 = 101 # 'e'
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500
            value3 = 'c' * 500
            value4 = 'd' * 500
            value5 = 'e' * 500

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        old_reader_session = self.conn.open_session()
        old_reader_cursor = old_reader_session.open_cursor(uri)
        old_reader_session.begin_transaction('read_timestamp=' + self.timestamp_str(20))

        # Now put two updates out of order. 5 will go to the history store and will trigger a
        # correction to the existing contents.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value4
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value5
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for i in range(1, 10000):
            self.assertEqual(cursor[self.get_key(i)], value4)
        self.session.rollback_transaction()

        for i in range(1, 10000):
            self.assertEqual(old_reader_cursor[self.get_key(i)], value2)
        old_reader_session.rollback_transaction()

    # In this test we're using modifies since they are more sensitive to corruptions.
    #
    # Corruptions to string types may go undetected since non-ASCII characters won't be included in
    # the conversion to a Python string.
    def test_timestamp20_modify(self):
        # FLCS does not support modifies, so skip this test.
        # Just return instead of using self.skipTest to avoid generating noise.
        if self.value_format == '8t':
            return

        uri = 'table:test_timestamp20'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        value1 = 'a' * 500
        value2 = 'b' * 500
        value3 = 'c' * 500

        # Apply the base value.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Now apply a series of modifies.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor.set_key(self.get_key(i))
            self.assertEqual(cursor.modify([wiredtiger.Modify('B', 100, 1)]), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor.set_key(self.get_key(i))
            self.assertEqual(cursor.modify([wiredtiger.Modify('C', 200, 1)]), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Open an old reader at this point.
        #
        # I'm trying to read the middle modify because I specifically don't want to read one that
        # has been squashed into a full update.
        old_reader_session = self.conn.open_session()
        old_reader_cursor = old_reader_session.open_cursor(uri)
        old_reader_session.begin_transaction('read_timestamp=' + self.timestamp_str(20))

        # Now apply the last modify.
        # This will be the end of the chain of modifies.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor.set_key(self.get_key(i))
            self.assertEqual(cursor.modify([wiredtiger.Modify('D', 300, 1)]), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        # Now put two updates out of order. 5 will go to the history store and will trigger a
        # correction to the existing contents.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))

        # Open up a new transaction and read at 30.
        # We shouldn't be able to see past 5 due to txnid visibility.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for i in range(1, 10000):
            self.assertEqual(cursor[self.get_key(i)], value2)
        self.session.rollback_transaction()

        # Put together expected value.
        expected = list(value1)
        expected[100] = 'B'
        expected = str().join(expected)

        # On the other hand, this older transaction SHOULD be able to read past the 5.
        for i in range(1, 10000):
            self.assertEqual(old_reader_cursor[self.get_key(i)], expected)
        old_reader_session.rollback_transaction()
