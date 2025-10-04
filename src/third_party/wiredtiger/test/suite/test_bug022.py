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
# test_bug022.py
#       Testing that we don't allow modifies on top of tombstone updates.

import wiredtiger, wttest
from wtscenario import make_scenarios

class test_bug022(wttest.WiredTigerTestCase):
    uri = 'file:test_bug022'
    conn_config = 'cache_size=50MB'

    key_format_values = [
        ('string-row', dict(key_format='S', usestrings=True)),
        ('column', dict(key_format='r', usestrings=False)),
    ]
    scenarios = make_scenarios(key_format_values)

    def get_key(self, i):
        return str(i) if self.usestrings else i

    def test_apply_modifies_on_onpage_tombstone(self):
        self.session.create(self.uri, 'key_format={},value_format=S'.format(self.key_format))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(self.uri)

        value = 'a' * 500
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[self.get_key(i)] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Apply tombstones for every key.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor.set_key(self.get_key(i))
            cursor.remove()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.session.checkpoint()

        # Now try to apply a modify on top of the tombstone at timestamp 3.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor.set_key(self.get_key(i))
            self.assertEqual(cursor.modify([wiredtiger.Modify('B', 0, 100)]), wiredtiger.WT_NOTFOUND)
            self.session.rollback_transaction()

        # Check that the tombstone is visible.
        for i in range(1, 10000):
            cursor.set_key(self.get_key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
