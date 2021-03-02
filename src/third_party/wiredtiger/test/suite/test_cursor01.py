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

# test_cursor01.py
#    Cursor operations
# Basic smoke-test of file and table cursors: tests get/set key, insert
# and forward/backward iteration, and mostly because we don't test them
# anywhere else, cursor duplication and equality.
class test_cursor01(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_cursor01'
    nentries = 10

    scenarios = make_scenarios([
        ('file-col', dict(tablekind='col',uri='file')),
        ('file-fix', dict(tablekind='fix',uri='file')),
        ('file-row', dict(tablekind='row',uri='file')),
        ('lsm-row', dict(tablekind='row',uri='lsm')),
        ('table-col', dict(tablekind='col',uri='table')),
        ('table-fix', dict(tablekind='fix',uri='table')),
        ('table-row', dict(tablekind='row',uri='table'))
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
    def create_session_and_cursor(self):
        tablearg = self.uri + ':' + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat

        self.pr('creating session: ' + create_args)
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor(tablearg, None, None)
        self.assertCursorHasNoKeyValue(cursor)
        self.assertEqual(cursor.uri, tablearg)

        for i in range(0, self.nentries):
            cursor[self.genkey(i)] = self.genvalue(i)

        return cursor

    # Forward iteration.
    def forward_iter(self, cursor):
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = 0
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1

        self.assertEqual(i, self.nentries)
        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)
        return cursor

    # Forward iteration with cursor duplication.
    def forward_iter_with_dup(self, cursor):
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = 0
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            dupc = self.session.open_cursor(None, cursor, None)
            self.assertEquals(cursor.compare(dupc), 0)
            key = dupc.get_key()
            value = dupc.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1
            cursor.close()
            cursor = dupc

        self.assertEqual(i, self.nentries)
        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)
        return cursor

    # Forward iteration through the object.
    # Don't use the builtin 'for ... in cursor', iterate using the basic API.
    def test_forward_iter(self):
        cursor = self.create_session_and_cursor()
        cursor = self.forward_iter(cursor)
        cursor = self.forward_iter_with_dup(cursor)
        cursor.close()

    # Backward iteration.
    def backward_iter(self, cursor):
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = self.nentries - 1
        while True:
            prevret = cursor.prev()
            if prevret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i -= 1

        self.assertEqual(i, -1)
        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)
        return cursor

    # Backward iteration with cursor duplication.
    def backward_iter_with_dup(self, cursor):
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = self.nentries - 1
        while True:
            prevret = cursor.prev()
            if prevret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i -= 1
            dupc = self.session.open_cursor(None, cursor, None)
            self.assertEquals(cursor.compare(dupc), 0)
            cursor.close()
            cursor = dupc

        self.assertEqual(i, -1)
        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)
        return cursor

    # Backward iteration through the object.
    # Don't use the builtin 'for ... in cursor', iterate using the basic API.
    def test_backward_iter(self):
        cursor = self.create_session_and_cursor()
        cursor = self.backward_iter(cursor)
        cursor = self.backward_iter_with_dup(cursor)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
