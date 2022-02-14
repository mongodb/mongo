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
# test_dupc.py
#       test cursor duplication
#

import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# Test session.open_cursor with cursor duplication.
class test_duplicate_cursor(wttest.WiredTigerTestCase):
    name = 'test_dupc'
    nentries = 1000

    scenarios = make_scenarios([
        ('file-f', dict(uri='file:', keyfmt='r', valfmt='8t')),
        ('file-r', dict(uri='file:', keyfmt='r', valfmt='S')),
        ('file-S', dict(uri='file:', keyfmt='S', valfmt='S')),
        ('table-f', dict(uri='table:', keyfmt='r', valfmt='8t')),
        ('table-r', dict(uri='table:', keyfmt='r', valfmt='S')),
        ('table-S', dict(uri='table:', keyfmt='S', valfmt='S'))
    ])

    # Iterate through an object, duplicate the cursor and checking that it
    # matches the original and is set to the same record.
    def iterate(self, uri, ds):
        cursor = self.session.open_cursor(uri, None, None)
        next = 0
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            next += 1
            self.assertEqual(cursor.get_key(), ds.key(next))
            dupc = self.session.open_cursor(None, cursor, None)
            self.assertEqual(cursor.compare(dupc), 0)
            self.assertEqual(dupc.get_key(), ds.key(next))
            cursor.close()
            cursor = dupc
        self.assertEqual(next, self.nentries)
        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_duplicate_cursor(self):
        uri = self.uri + self.name

        # A simple, one-file file or table object.
        ds = SimpleDataSet(self, uri, self.nentries, \
                key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        self.iterate(uri, ds)
        self.dropUntilSuccess(self.session, uri)

        # A complex, multi-file table object.
        if self.uri == "table:" and self.valfmt != '8t':
            ds = ComplexDataSet(self, uri, self.nentries, \
                    key_format=self.keyfmt, value_format=self.valfmt)
            ds.populate()
            self.iterate(uri, ds)
            self.dropUntilSuccess(self.session, uri)

if __name__ == '__main__':
    wttest.run()
