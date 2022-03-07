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
from wtscenario import make_scenarios

'''
 test_hs22.py
 Test the following cases with out of order(OOO) timestamps:
 - OOO update followed by a tombstone.
 - Multiple OOO updates in a single transaction.
 - Most recent OOO updates that require squashing.
'''
class test_hs22(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'

    format_values = [
        ('column', dict(key_format='r', key1=1, key2=2, value_format='S')),
        ('column-fix', dict(key_format='r', key1=1, key2=2, value_format='8t')),
        ('string-row', dict(key_format='S', key1=str(0), key2=str(1), value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_onpage_out_of_order_timestamp_update(self):
        uri = 'table:test_hs22'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        key1 = self.key1
        key2 = self.key2

        if self.value_format == '8t':
            value1 = 97 # 'a'
            value2 = 98 # 'b'
        else:
            value1 = 'a'
            value2 = 'b'

        # Insert a key.
        self.session.begin_transaction()
        cursor[key1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Remove the key.
        self.session.begin_transaction()
        cursor.set_key(key1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Do an out of order timestamp
        # update and write it to the data
        # store later.
        self.session.begin_transaction()
        cursor[key1] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))

        # Insert another key.
        self.session.begin_transaction()
        cursor[key2] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Update the key.
        self.session.begin_transaction()
        cursor[key2] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Do a checkpoint to trigger
        # history store reconciliation.
        self.session.checkpoint()

        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search the key to evict it.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(15))
        self.assertEqual(evict_cursor[key1], value2)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Search the key again to verify the data is still as expected.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(15))
        self.assertEqual(cursor[key1], value2)
        self.session.rollback_transaction()

    def test_out_of_order_timestamp_update_newer_than_tombstone(self):
        uri = 'table:test_hs22'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        key1 = self.key1
        key2 = self.key2

        if self.value_format == '8t':
            value1 = 97 # 'a'
            value2 = 98 # 'b'
        else:
            value1 = 'a'
            value2 = 'b'

        # Insert a key.
        self.session.begin_transaction()
        cursor[key1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Remove a key.
        self.session.begin_transaction()
        cursor.set_key(key1)
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Do an out of order timestamp
        # update and write it to the
        # history store later.
        self.session.begin_transaction()
        cursor[key1] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))

        # Add another update.
        self.session.begin_transaction()
        cursor[key1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Insert another key.
        self.session.begin_transaction()
        cursor[key2] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Update the key.
        self.session.begin_transaction()
        cursor[key2] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Do a checkpoint to trigger
        # history store reconciliation.
        self.session.checkpoint()

        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search the key to evict it.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(15))
        self.assertEqual(evict_cursor[key1], value2)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Search the key again to verify the data is still as expected.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(15))
        self.assertEqual(cursor[key1], value2)
        self.session.rollback_transaction()

    def test_out_of_order_timestamp_update_same_txn(self):
        uri = 'table:test_hs22'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        key1 = self.key1

        if self.value_format == '8t':
            value1 = 97  # 'a'
            value2 = 98  # 'b'
            value3 = 99  # 'c'
            value4 = 100 # 'd'
        else:
            value1 = 'a'
            value2 = 'b'
            value3 = 'c'
            value4 = 'd'

        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor[key1] = value1
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
        cursor[key1] = value2
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(4))
        cursor[key1] = value3
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
        cursor[key1] = value4
        self.session.commit_transaction()

        # Do a checkpoint to trigger
        # history store reconciliation.
        self.session.checkpoint()

        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search the key to evict it.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10))
        self.assertEqual(evict_cursor[key1], value4)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Search the key again to verify the data is still as expected.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10))
        self.assertEqual(cursor[key1], value4)
        self.session.rollback_transaction()

    def test_out_of_order_timestamp_squash_updates(self):
        uri = 'table:test_hs22'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        key1 = self.key1

        if self.value_format == '8t':
            value1 = 97  # 'a'
            value2 = 98  # 'b'
            value3 = 99  # 'c'
            value4 = 100 # 'd'
            value5 = 101 # 'e'
        else:
            value1 = 'a'
            value2 = 'b'
            value3 = 'c'
            value4 = 'd'
            value5 = 'e'

        self.session.begin_transaction()
        cursor[key1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))

        self.session.begin_transaction()
        cursor[key1] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(18))

        self.session.begin_transaction()
        cursor.set_key(key1)
        cursor.remove()
        cursor[key1] = value3
        cursor[key1] = value4
        cursor[key1] = value5
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(11))

        # Do a checkpoint to trigger
        # history store reconciliation.
        self.session.checkpoint()

        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search the key to evict it.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(18))
        self.assertEqual(evict_cursor[key1], value5)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Search the key again to verify the data is still as expected.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(18))
        self.assertEqual(cursor[key1], value5)
        self.session.rollback_transaction()
