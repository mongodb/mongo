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
# transactions:mixed_mode_timestamps
# verify:prepare
# [END_TAGS]
#
# test_timestamp18.py
#   Mixing timestamped and non-timestamped writes.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp18(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'

    key_format_values = [
        ('string-row', dict(key_format='S', usestrings=True)),
        ('column', dict(key_format='r', usestrings=False)),
    ]
    non_ts_writes = [
        ('insert', dict(delete=False)),
        ('delete', dict(delete=True)),
    ]
    scenarios = make_scenarios(key_format_values, non_ts_writes)

    def get_key(self, i):
        return str(i) if self.usestrings else i

    def test_ts_writes_with_non_ts_write(self):
        uri = 'table:test_timestamp18'
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        value1 = 'a' * 500
        value2 = 'b' * 500
        value3 = 'c' * 500
        value4 = 'd' * 500

        # A series of timestamped writes on each key.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        # Add a non-timestamped delete.
        # Let's do every second key to ensure that we get the truncation right and don't
        # accidentally destroy content from an adjacent key.
        for i in range(1, 10000):
            if i % 2 == 0:
                if self.delete:
                    cursor.set_key(self.get_key(i))
                    cursor.remove()
                else:
                    cursor[self.get_key(i)] = value4

        self.session.checkpoint()

        for ts in range(2, 4):
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
            for i in range(1, 10000):
                # The non-timestamped delete should cover all the previous writes and make them effectively
                # invisible.
                if i % 2 == 0:
                    if self.delete:
                        cursor.set_key(self.get_key(i))
                        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                    else:
                        self.assertEqual(cursor[self.get_key(i)], value4)
                # Otherwise, expect one of the timestamped writes.
                else:
                    if ts == 2:
                        self.assertEqual(cursor[self.get_key(i)], value1)
                    elif ts == 3:
                        self.assertEqual(cursor[self.get_key(i)], value2)
                    else:
                        self.assertEqual(cursor[self.get_key(i)], value3)
            self.session.rollback_transaction()
