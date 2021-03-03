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

# test_index01.py
#    basic tests for indices
class test_index01(wttest.WiredTigerTestCase):
    '''Test basic operations for indices'''

    basename = 'test_index01'
    tablename = 'table:' + basename
    indexbase = 'index:' + basename
    NUM_INDICES = 6

    def create_table(self):
        self.index = ['%s:index%d' % (self.indexbase, i) \
            for i in range(self.NUM_INDICES)]
        self.pr('create table')
        self.session.create(self.tablename, 'key_format=Si,value_format=SSii,columns=(name,ID,dept,job,salary,year)')
        self.session.create(self.index[0], 'columns=(dept)')
        self.session.create(self.index[1], 'columns=(name,year)')
        self.session.create(self.index[2], 'columns=(salary)')
        self.session.create(self.index[3], 'columns=(dept,job,name)')
        self.session.create(self.index[4], 'columns=(name,ID)')
        self.session.create(self.index[5], 'columns=(ID,name)')

    def drop_table(self):
        self.pr('drop table')
        self.session.drop(self.tablename, None)

    def cursor(self, config=None):
        self.pr('open cursor')
        c = self.session.open_cursor(self.tablename, None, config)
        self.assertNotEqual(c, None)
        return c

    def index_cursor(self, i):
        self.pr('open index cursor(%d)' % i)
        c = self.session.open_cursor(self.index[i], None, None)
        self.assertNotEqual(c, None)
        return c

    def index_iter(self, i):
        cursor = self.index_cursor(i)
        for cols in cursor:
            yield cols
        cursor.close()

    def check_exists(self, name, ID, expected):
        cursor = self.cursor()
        cursor.set_key(name, ID)
        self.pr('search')
        self.assertEqual(cursor.search(), expected)
        self.pr('closing cursor')
        cursor.close()

    def insert(self, *cols):
        self.pr('insert')
        cursor = self.cursor(config='overwrite=false')
        cursor.set_key(*cols[:2])
        cursor.set_value(*cols[2:])
        self.assertEqual(cursor.insert(), 0)
        cursor.close()

    def insert_duplicate(self, *cols):
        self.pr('insert')
        cursor = self.cursor(config='overwrite=false')
        cursor.set_key(*cols[:2])
        cursor.set_value(*cols[2:])
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())
        cursor.close()

    def insert_overwrite(self, *cols):
        self.pr('insert')
        cursor = self.cursor(config='overwrite=true')
        cursor.set_key(*cols[:2])
        cursor.set_value(*cols[2:])
        self.assertEqual(cursor.insert(), 0)
        cursor.close()

    def update(self, *cols):
        self.pr('update')
        cursor = self.cursor(config='overwrite=false')
        cursor.set_key(*cols[:2])
        cursor.set_value(*cols[2:])
        self.assertEqual(cursor.update(), 0)
        cursor.close()

    def update_nonexistent(self, *cols):
        self.pr('update')
        cursor = self.cursor(config='overwrite=false')
        cursor.set_key(*cols[:2])
        cursor.set_value(*cols[2:])
        self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def remove(self, name, ID):
        self.pr('remove')
        cursor = self.cursor(config='overwrite=false')
        cursor.set_key(name, ID)
        self.assertEqual(cursor.remove(), 0)
        cursor.close()

    def test_empty(self):
        '''Create a table, look for a nonexistent key'''
        self.create_table()
        self.check_exists('jones', 10, wiredtiger.WT_NOTFOUND)
        for i in range(self.NUM_INDICES):
            self.assertEqual(list(self.index_iter(i)), [])
        self.drop_table()

    def test_insert(self):
        '''Create a table, add a key, get it back'''
        self.create_table()
        self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
        self.check_exists('smith', 1, 0)
        result = ''
        for i in range(self.NUM_INDICES):
            result += '\n'.join(repr(cols)
                for cols in self.index_iter(i))
            result += '\n\n'
        self.assertEqual(result, \
            "['HR', 'HR', 'manager', 100000, 1970]\n\n" + \
            "['smith', 1970, 'HR', 'manager', 100000, 1970]\n\n" + \
            "[100000, 'HR', 'manager', 100000, 1970]\n\n" + \
            "['HR', 'manager', 'smith', 'HR', 'manager', 100000, 1970]\n\n" + \
            "['smith', 1, 'HR', 'manager', 100000, 1970]\n\n" + \
            "[1, 'smith', 'HR', 'manager', 100000, 1970]\n\n")
        self.drop_table()

    def test_update(self):
        '''Create a table, add a key, update it, get it back'''
        self.create_table()
        self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
        self.update('smith', 1, 'HR', 'janitor', 10000, 1970)
        self.update_nonexistent('smith', 2, 'HR', 'janitor', 1000, 1970)
        self.update_nonexistent('Smith', 1, 'HR', 'janitor', 1000, 1970)
        self.check_exists('smith', 1, 0)
        result = ''
        for i in range(self.NUM_INDICES):
            result += '\n'.join(repr(cols)
                for cols in self.index_iter(i))
            result += '\n\n'
        self.assertEqual(result, \
            "['HR', 'HR', 'janitor', 10000, 1970]\n\n" + \
            "['smith', 1970, 'HR', 'janitor', 10000, 1970]\n\n" + \
            "[10000, 'HR', 'janitor', 10000, 1970]\n\n" + \
            "['HR', 'janitor', 'smith', 'HR', 'janitor', 10000, 1970]\n\n" + \
            "['smith', 1, 'HR', 'janitor', 10000, 1970]\n\n" + \
            "[1, 'smith', 'HR', 'janitor', 10000, 1970]\n\n")
        self.drop_table()

    def test_insert_overwrite(self):
        '''Create a table, add a key, insert-overwrite it,
           insert-overwrite a nonexistent record, get them both back'''
        self.create_table()
        self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
        self.insert_overwrite('smith', 1, 'HR', 'janitor', 10000, 1970)
        self.insert_overwrite('jones', 2, 'IT', 'sysadmin', 50000, 1980)
        self.check_exists('smith', 1, 0)
        self.check_exists('jones', 2, 0)
        self.insert_duplicate('smith', 1, 'HR', 'manager', 100000, 1970)
        result = ''
        for i in range(self.NUM_INDICES):
            result += '\n'.join(repr(cols)
                for cols in self.index_iter(i))
            result += '\n\n'
        self.assertEqual(result, \
            "['HR', 'HR', 'janitor', 10000, 1970]\n" + \
            "['IT', 'IT', 'sysadmin', 50000, 1980]\n\n" + \
            "['jones', 1980, 'IT', 'sysadmin', 50000, 1980]\n" + \
            "['smith', 1970, 'HR', 'janitor', 10000, 1970]\n\n" + \
            "[10000, 'HR', 'janitor', 10000, 1970]\n" + \
            "[50000, 'IT', 'sysadmin', 50000, 1980]\n\n" + \
            "['HR', 'janitor', 'smith', 'HR', 'janitor', 10000, 1970]\n" + \
            "['IT', 'sysadmin', 'jones', 'IT', 'sysadmin', 50000, 1980]\n\n" + \
            "['jones', 2, 'IT', 'sysadmin', 50000, 1980]\n" + \
            "['smith', 1, 'HR', 'janitor', 10000, 1970]\n\n" + \
            "[1, 'smith', 'HR', 'janitor', 10000, 1970]\n" + \
            "[2, 'jones', 'IT', 'sysadmin', 50000, 1980]\n\n")

        self.drop_table()

    def test_insert_delete(self):
        '''Create a table, add a key, remove it'''
        self.create_table()
        self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
        self.check_exists('smith', 1, 0)
        self.remove('smith', 1)
        self.check_exists('smith', 1, wiredtiger.WT_NOTFOUND)
        for i in range(self.NUM_INDICES):
            self.assertEqual(list(self.index_iter(i)), [])
        self.drop_table()

    def test_exclusive(self):
        '''Create indices, then try to create another index exclusively'''
        self.create_table()
        # non-exclusive recreate is allowed
        self.session.create(self.index[0], 'columns=(dept)')
        # exclusive recreate
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.index[0],
            'columns=(dept),exclusive'))
        self.drop_table()

if __name__ == '__main__':
    wttest.run()
