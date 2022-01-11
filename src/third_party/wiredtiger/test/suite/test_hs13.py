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

# test_hs13.py
# Verify reverse modify traversal after eviction.
class test_hs13(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB,eviction=(threads_max=1)'
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer-row', dict(key_format='i'))
    ]
    scenarios = make_scenarios(key_format_values)

    def test_reverse_modifies_constructed_after_eviction(self):
        uri = "table:test_hs13"
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        value1 = 'a' * 10000
        value2 = 'b' * 10000
        value3 = 'e' * 10000

        self.session.create(uri, create_params)
        cursor = self.session.open_cursor(uri)
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)

        # Insert a full value.
        self.session.begin_transaction()
        cursor[1] = value1
        self.session.commit_transaction()

        # Insert a modify.
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.modify([wiredtiger.Modify('A', 0, 0)])
        self.session.commit_transaction()

        # Validate that we do see the correct value.
        session2.begin_transaction()
        cursor2.set_key(1)
        cursor2.search()
        self.assertEquals(cursor2.get_value(),  'A' + value1)
        session2.commit_transaction()

        # Reset the cursor.
        cursor2.reset()

        # Begin session2's transaction so it gets a snapshot prior to the full value being
        # inserted below.
        session2.begin_transaction()

        # Insert a second modify.
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.modify([wiredtiger.Modify('B', 1, 0)])
        self.session.commit_transaction()

        # Insert one more value.
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor[1] = value2
        self.session.commit_transaction()

        # Configure debug behavior on a cursor to evict the positioned page on cursor reset
        # and evict the page.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        evict_cursor.set_key(1)
        self.assertEquals(evict_cursor.search(), 0)
        evict_cursor.reset()

        # Try to find the value we saw earlier.
        cursor2.set_key(1)
        cursor2.search()
        self.assertEquals(cursor2.get_value(), 'A' + value1)

if __name__ == '__main__':
    wttest.run()
