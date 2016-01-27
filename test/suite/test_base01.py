#!/usr/bin/env python
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

# test_base01.py
#    Basic operations
class test_base01(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_base01a.wt'
    table_name2 = 'test_base01b.wt'

    def create_table(self, tablename):
        extra_params = ',allocation_size=512,' +\
            'internal_page_max=16384,leaf_page_max=131072'
        self.pr('create_table')
        self.session.create('table:' + tablename,
            'key_format=S,value_format=S' + extra_params)

    def cursor_s(self, tablename, key):
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        cursor.set_key(key)
        return cursor

    def cursor_ss(self, tablename, key, val):
        cursor = self.cursor_s(tablename, key)
        cursor.set_value(val)
        return cursor

    def test_error(self):
        gotException = False
        expectMessage = 'unknown configuration key'
        with self.expectedStderrPattern(expectMessage):
            try:
                self.pr('expect an error message...')
                self.session.create('table:' + self.table_name1,
                                    'expect_this_error,okay?')
            except wiredtiger.WiredTigerError as e:
                gotException = True
                self.pr('got expected exception: ' + str(e))
                self.assertTrue(str(e).find('nvalid argument') >= 0)
        self.assertTrue(gotException, 'expected exception')

    def test_empty(self):
        """
        Create a table, look for a nonexistent key
        """
        self.create_table(self.table_name1)
        self.pr('creating cursor')
        cursor = self.cursor_s(self.table_name1, 'somekey')
        self.pr('search')
        ret = cursor.search()
        self.assertTrue(ret == wiredtiger.WT_NOTFOUND)
        self.pr('closing cursor')
        cursor.close()

    def test_insert(self):
        """
        Create a table, add a key, get it back
        """
        self.create_table(self.table_name2)

        self.pr('insert')
        inscursor = self.cursor_ss(self.table_name2, 'key1', 'value1')
        inscursor.insert()
        inscursor.close

        self.pr('search')
        getcursor = self.cursor_s(self.table_name2, 'key1')
        ret = getcursor.search()
        self.assertTrue(ret == 0)
        self.assertTrue(getcursor.get_value(), 'value1')
        self.pr('closing cursor')
        getcursor.close()

if __name__ == '__main__':
    wttest.run()
