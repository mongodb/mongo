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
# history_store:eviction_checkpoint_interaction
# [END_TAGS]
#

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_hs15.py
# Ensure eviction doesn't clear the history store again after checkpoint has done so because of the same update without timestamp.
class test_hs15(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=5MB'
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='S'))
    ]
    scenarios = make_scenarios(format_values)

    def create_key(self, i):
        if self.key_format == 'S':
            return str(i)
        return i

    def test_hs15(self):
        uri = 'table:test_hs15'
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)
        cursor = self.session.open_cursor(uri)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
            value3 = 99
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500
            value3 = 'c' * 500

        # Insert an update without timestamp
        self.session.begin_transaction()
        cursor[self.create_key(1)] = value1
        self.session.commit_transaction()

        # Insert a bunch of other contents to trigger eviction
        for i in range(2, 1000):
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Do a modify and an update with timestamps (for FLCS, just modifies)
        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        if self.value_format == '8t':
           cursor.set_value(66)
           self.assertEqual(cursor.update(), 0)
        else:
           mods = [wiredtiger.Modify('B', 100, 1)]
           self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        self.session.begin_transaction()
        cursor[self.create_key(1)] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Make the modify with timestamp and the update without timestamp obsolete
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Do a checkpoint
        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[self.create_key(1)] = value3
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Insert a bunch of other contents to trigger eviction
        for i in range(2, 1000):
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        if self.value_format == '8t':
            expected = 66 # 'B'
        else:
            expected = list(value1)
            expected[100] = 'B'
            expected = str().join(expected)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(1))
        self.assertEqual(cursor[self.create_key(1)], expected)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(2))
        self.assertEqual(cursor[self.create_key(1)], value2)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(3))
        self.assertEqual(cursor[self.create_key(1)], value3)
        self.session.rollback_transaction()
