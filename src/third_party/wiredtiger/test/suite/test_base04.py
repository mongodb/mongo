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

# test_base04.py
#    Test that tables are reconciled correctly when they are empty.
class test_base04(wttest.WiredTigerTestCase):
    '''Test various tree types becoming empty'''

    tablename = 'table:test_base04'

    def __init__(self, *args, **kwargs):
        wttest.WiredTigerTestCase.__init__(self, *args, **kwargs)
        self.reconcile = False

    def create_table(self):
        self.pr('create table')
        self.session.create(
            self.tablename, 'key_format=S,value_format=S')

    def drop_table(self):
        self.pr('drop table')
        self.dropUntilSuccess(self.session, self.tablename)

    def cursor(self):
        self.pr('open cursor')
        return self.session.open_cursor(self.tablename, None, None)

    def check_exists(self, key, expected):
        cursor = self.cursor()
        cursor.set_key(key)
        self.pr('search')
        self.assertEqual(cursor.search(), expected)
        self.pr('closing cursor')
        cursor.close()

    def insert(self, key, value):
        self.pr('insert')
        cursor = self.cursor()
        cursor[key] = value
        cursor.close()
        if self.reconcile:
            self.reopen_conn()

    def remove(self, key):
        self.pr('remove')
        cursor = self.cursor()
        cursor.set_key(key)
        cursor.remove()
        cursor.close()
        if self.reconcile:
            self.reopen_conn()

    def test_empty(self):
        '''Create a table, look for a nonexistent key'''
        self.create_table()
        self.check_exists('somekey', wiredtiger.WT_NOTFOUND)
        self.drop_table()

    def test_insert(self):
        '''Create a table, add a key, get it back'''
        for self.reconcile in (False, True):
            self.create_table()
            self.insert('key1', 'value1')
            self.check_exists('key1', 0)
            self.drop_table()

    def test_insert_delete(self):
        '''Create a table, add a key, get it back'''
        for reconcile in (False, True):
            self.create_table()
            self.insert('key1', 'value1')
            self.check_exists('key1', 0)
            self.remove('key1')
            self.check_exists('key1', wiredtiger.WT_NOTFOUND)
            self.drop_table()

if __name__ == '__main__':
    wttest.run()
