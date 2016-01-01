#!usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
from helper import simple_populate, key_populate, value_populate
from wtscenario import check_scenarios

# test_cursor_pin.py
#       Smoke-test fast-path searching for pinned pages before re-descending
# the tree.
class test_cursor_pin(wttest.WiredTigerTestCase):
    uri = 'file:cursor_pin'
    nentries = 10000
    config = 'allocation_size=512,leaf_page_max=512,value_format=S,key_format='
    scenarios = check_scenarios([
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S')),
    ])

    # Create a multi-page file, confirm that a simple search to the local
    # page works, followed by a search to a different page.
    def test_smoke(self):
        simple_populate(self,
            self.uri, self.config + self.keyfmt, self.nentries)
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        c.set_key(key_populate(c, 100))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), value_populate(c, 100))
        c.set_key(key_populate(c, 101))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), value_populate(c, 101))
        c.set_key(key_populate(c, 9999))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), value_populate(c, 9999))

    # Forward check.
    def forward(self, c, max, notfound):
        for i in range(1, max + 1):
            c.set_key(key_populate(c, i))
            if i in notfound:
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(c.search(), 0)
                self.assertEqual(c.get_value(), value_populate(c, i))

    # Backward check.
    def backward(self, c, max, notfound):
        for i in range(max, 0, -1):
            c.set_key(key_populate(c, i))
            if i in notfound:
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(c.search(), 0)
                self.assertEqual(c.get_value(), value_populate(c, i))

    # Create a multi-page file, search backward, forward to check page
    # boundaries.
    def test_basic(self):
        simple_populate(self,
            self.uri, self.config + self.keyfmt, self.nentries)
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        self.forward(c, self.nentries, [])
        self.backward(c, self.nentries, [])

    # Create a multi-page file with a big chunk of missing space in the
    # middle (to exercise column-store searches).
    def test_missing(self):
        simple_populate(self,
            self.uri, self.config + self.keyfmt, self.nentries)
        c = self.session.open_cursor(self.uri, None)
        for i in range(self.nentries + 3000, self.nentries + 5001):
            c[key_populate(c, i)] = value_populate(c, i)
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        self.forward(c, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 3000)))
        self.backward(c, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 3000)))

        # Insert into the empty space so we test searching inserted items.
        for i in range(self.nentries + 1000, self.nentries + 2001):
            c[key_populate(c, i)] = value_populate(c, i)
        self.forward(c, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 1000) +\
                 range(self.nentries + 2001, self.nentries + 3000)))
        self.backward(c, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 1000) +\
                 range(self.nentries + 2001, self.nentries + 3000)))


if __name__ == '__main__':
    wttest.run()
