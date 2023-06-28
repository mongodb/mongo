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

# test_metadata_cursor01.py
#    Metadata cursor operations
# Basic smoke-test of metadata cursor: test backward and forward iteration
# as well as search.
class test_metadata_cursor01(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_metadata_cursor01'

    scenarios = make_scenarios([
        ('plain', {'metauri' : 'metadata:'}),
        ('create', {'metauri' : 'metadata:create'}),
    ])

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i)
        else:
            return self.recno(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    def assertCursorHasNoKeyValue(self, cursor):
        keymsg = '/requires key be set/'
        valuemsg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.get_key, keymsg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.get_value, valuemsg)

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    # Create and populate the object, returning an open cursor.
    def create_table(self):
        tablearg = 'table:' + self.table_name1
        create_args = 'key_format=S,value_format=S'

        self.pr('creating session: ' + create_args)
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')

    # Forward iteration.
    def test_forward_iter(self):
        self.create_table()
        cursor = self.session.open_cursor(self.metauri, None, None)
        self.assertCursorHasNoKeyValue(cursor)

        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            self.assertIsNotNone(cursor.get_key())
            self.assertIsNotNone(cursor.get_value())

        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)
        cursor.close()

    # Backward iteration.
    def test_backward_iter(self):
        self.create_table()
        cursor = self.session.open_cursor(self.metauri, None, None)
        self.assertCursorHasNoKeyValue(cursor)

        while True:
            prevret = cursor.prev()
            if prevret != 0:
                break
            self.assertIsNotNone(cursor.get_key())
            self.assertIsNotNone(cursor.get_value())

        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)
        cursor.close()

    # Test search
    def test_search(self):
        self.create_table()
        cursor = self.session.open_cursor(self.metauri, None, None)
        self.assertCursorHasNoKeyValue(cursor)

        # Ensure the 'special' metadata is found.
        value = cursor['metadata:']
        self.assertTrue(value.find('key_format') != -1)

        # Ensure the metadata for the table we created is found
        value = cursor['table:' + self.table_name1]
        self.assertTrue(value.find('key_format') != -1)

if __name__ == '__main__':
    wttest.run()
