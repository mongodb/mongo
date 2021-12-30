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
# test_empty_value.py
#       Smoke test empty row-store values.

from wiredtiger import stat
import wiredtiger, wttest, unittest

# Smoke test empty row-store values.
class test_row_store_empty_values(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    # Smoke test empty row-store values.
    def test_row_store_empty_values(self):
        nentries = 25000
        uri = 'file:test_empty_values'          # This is a btree layer test.

        # Create the object, open the cursor, insert some records with zero-length values.
        self.session.create(uri, 'key_format=S,value_format=u')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, nentries + 1):
            cursor[str(i)] = b''
        cursor.close()

        # Reopen to force the object to disk.
        self.reopen_conn()

        # Confirm the values weren't stored.
        cursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(tree_walk)')
        self.assertEqual(cursor[stat.dsrc.btree_row_empty_values][2], nentries)

if __name__ == '__main__':
    wttest.run()
