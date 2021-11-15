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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_cursor_pin.py
#       Smoke-test fast-path searching for pinned pages before re-descending
# the tree.
class test_cursor_pin(wttest.WiredTigerTestCase):
    uri = 'file:cursor_pin'
    nentries = 10000
    config = 'allocation_size=512,leaf_page_max=512'
    scenarios = make_scenarios([
        ('recno-fix', dict(keyfmt='r', valfmt='8t')),
        ('recno', dict(keyfmt='r', valfmt='S')),
        ('string', dict(keyfmt='S', valfmt='S')),
    ])

    # Create a multi-page file, confirm that a simple search to the local
    # page works, followed by a search to a different page.
    def test_smoke(self):
        ds = SimpleDataSet(self, self.uri, self.nentries,
            config=self.config, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        c.set_key(ds.key(100))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), ds.value(100))
        c.set_key(ds.key(101))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), ds.value(101))
        c.set_key(ds.key(9999))
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), ds.value(9999))

    # Forward check.
    def forward(self, c, ds, max, notfound):
        for i in range(1, max + 1):
            c.set_key(ds.key(i))
            if i in notfound:
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(c.search(), 0)
                self.assertEqual(c.get_value(), ds.value(i))

    # Backward check.
    def backward(self, c, ds, max, notfound):
        for i in range(max, 0, -1):
            c.set_key(ds.key(i))
            if i in notfound:
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(c.search(), 0)
                self.assertEqual(c.get_value(), ds.value(i))

    # Create a multi-page file, search backward, forward to check page
    # boundaries.
    def test_basic(self):
        ds = SimpleDataSet(self, self.uri, self.nentries,
            config=self.config, key_format=self.keyfmt)
        ds.populate()
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        self.forward(c, ds, self.nentries, [])
        self.backward(c, ds, self.nentries, [])

    # Create a multi-page file with a big chunk of missing space in the
    # middle (to exercise column-store searches).
    def test_missing(self):
        ds = SimpleDataSet(self, self.uri, self.nentries,
            config=self.config, key_format=self.keyfmt)
        ds.populate()
        c = self.session.open_cursor(self.uri, None)
        for i in range(self.nentries + 3000, self.nentries + 5001):
            c[ds.key(i)] = ds.value(i)
        self.reopen_conn()
        c = self.session.open_cursor(self.uri, None)
        self.forward(c, ds, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 3000)))
        self.backward(c, ds, self.nentries + 5000,
            list(range(self.nentries + 1, self.nentries + 3000)))

        # Insert into the empty space so we test searching inserted items.
        for i in range(self.nentries + 1000, self.nentries + 2001):
            c[ds.key(i)] = ds.value(i)
        self.forward(c, ds, self.nentries + 5000,
            list(list(range(self.nentries + 1, self.nentries + 1000)) +\
                 list(range(self.nentries + 2001, self.nentries + 3000))))
        self.backward(c, ds, self.nentries + 5000,
            list(list(range(self.nentries + 1, self.nentries + 1000)) +\
                 list(range(self.nentries + 2001, self.nentries + 3000))))

if __name__ == '__main__':
    wttest.run()
