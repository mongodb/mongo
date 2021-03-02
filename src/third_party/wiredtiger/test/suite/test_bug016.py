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

# test_bug016.py
#       WT-2757: WT_CURSOR.get_key() fails after WT_CURSOR.insert unless the
# cursor has a record number key with append configured.
class test_bug016(wttest.WiredTigerTestCase):

    # Insert a row into a simple column-store table configured to append.
    # WT_CURSOR.get_key should succeed.
    def test_simple_column_store_append(self):
        uri='file:bug016'
        self.session.create(uri, 'key_format=r,value_format=S')
        cursor = self.session.open_cursor(uri, None, 'append')
        cursor.set_value('value')
        cursor.insert()
        self.assertEquals(cursor.get_key(), 1)

    # Insert a row into a simple column-store table.
    # WT_CURSOR.get_key should fail.
    def test_simple_column_store(self):
        uri='file:bug016'
        self.session.create(uri, 'key_format=r,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(37)
        cursor.set_value('value')
        cursor.insert()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.get_key(), "/requires key be set/")

    # Insert a row into a simple row-store table.
    # WT_CURSOR.get_key should fail.
    def test_simple_row_store(self):
        uri='file:bug016'
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key('key')
        cursor.set_value('value')
        cursor.insert()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.get_key(), "/requires key be set/")

    # Insert a row into a complex column-store table configured to append.
    # WT_CURSOR.get_key should succeed.
    def test_complex_column_store_append(self):
        uri='table:bug016'
        self.session.create(
            uri, 'key_format=r,value_format=S,columns=(key,value)')
        cursor = self.session.open_cursor(uri, None, 'append')
        cursor.set_value('value')
        cursor.insert()
        self.assertEquals(cursor.get_key(), 1)

    # Insert a row into a complex column-store table.
    # WT_CURSOR.get_key should fail.
    def test_complex_column_store(self):
        uri='table:bug016'
        self.session.create(
            uri, 'key_format=r,value_format=S,columns=(key,value)')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(37)
        cursor.set_value('value')
        cursor.insert()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.get_key(), "/requires key be set/")

    # Insert a row into a complex row-store table.
    # WT_CURSOR.get_key should fail.
    def test_complex_row_store(self):
        uri='table:bug016'
        self.session.create(
            uri, 'key_format=S,value_format=S,columns=(key,value)')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key('key')
        cursor.set_value('value')
        cursor.insert()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.get_key(), "/requires key be set/")

if __name__ == '__main__':
    wttest.run()
