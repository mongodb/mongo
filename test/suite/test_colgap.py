#!usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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

# test_colgap.py
#    Test variable-length column-store gap performance.
class test_column_store_gap(wttest.WiredTigerTestCase):
    nentries = 13

    # Cursor forward
    def forward(self, cursor):
        cursor.reset()
        i = 0
        while True:
            if cursor.next() != 0:
                break
            i += 1
        self.assertEqual(i, self.nentries)

    # Cursor backward
    def backward(self, cursor):
        cursor.reset()
        i = 0
        while True:
            if cursor.prev() != 0:
                break
            i += 1
        self.assertEqual(i, self.nentries)

    # Create a variable-length column-store table with really big gaps in the
    # namespace. If this runs in less-than-glacial time, it's working.
    def test_column_store_gap(self):
        uri = 'table:gap'
        simple_populate(self, uri, 'key_format=r,value_format=S', 10)
        cursor = self.session.open_cursor(uri, None, None)

        # Create a column-store table with large gaps in the name-space.
        v = [ 1000, 2000000000000, 30000000000000 ]
        for i in v:
            cursor.set_key(key_populate(cursor, i))
            cursor.set_value(value_populate(cursor, i))
            cursor.insert()

        # In-memory cursor forward, backward.
        self.forward(cursor)
        self.backward(cursor)

        self.reopen_conn()
        cursor = self.session.open_cursor(uri, None, None)

        # Disk page cursor forward, backward.
        self.forward(cursor)
        self.backward(cursor)


if __name__ == '__main__':
    wttest.run()
