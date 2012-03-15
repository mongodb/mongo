#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
# test_cursor01.py
# 	Cursor operations
#

import wiredtiger, wttest

class test_cursor01(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_cursor01'
    nentries = 10

    scenarios = [
        ('row', dict(tablekind='row')),
        ('col', dict(tablekind='col')),
        ('fix', dict(tablekind='fix'))
        ]

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return ''

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    def create_session_and_cursor(self):
        tablearg = "table:" + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat + self.config_string()
        print('creating session: ' + create_args)
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')
        return self.session.open_cursor(tablearg, None, None)

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i)
        else:
            return long(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    def assertCursorHasNoKeyValue(self, cursor):
        keymsg = 'cursor.get_key: requires key be set: Invalid argument\n'
        valuemsg = 'cursor.get_value: requires value be set: Invalid argument\n'
        with self.expectedStderr(keymsg):
            self.assertRaises(wiredtiger.WiredTigerError, cursor.get_key)
        with self.expectedStderr(valuemsg):
            self.assertRaises(wiredtiger.WiredTigerError, cursor.get_value)
        
    def test_forward_iter(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        cursor = self.create_session_and_cursor()
        self.assertCursorHasNoKeyValue(cursor)

        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            cursor.set_value(self.genvalue(i))
            cursor.insert()

        # Don't use the builtin 'for ... in cursor',
        # iterate using the basic API.

        # 1. Start with the first k/v pair.
        cursor.reset()
        i = 0
        while True:
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            #print('want: ' + str(self.genkey(i)) + ' got: ' + str(key))
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1

        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, self.nentries)

        # After an error, we can no longer access the key or value
        self.assertCursorHasNoKeyValue(cursor)

        # 2. Setting reset() should place us just before first pair.
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)
            
        nextret = cursor.next()
        i = 0
        while nextret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1
            nextret = cursor.next()

        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, self.nentries)
        cursor.close()

    def test_backward_iter(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        cursor = self.create_session_and_cursor()
        self.assertCursorHasNoKeyValue(cursor)

        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            cursor.set_value(self.genvalue(i))
            cursor.insert()

        # Don't use the builtin 'for ... in cursor',
        # iterate using the basic API.

        # 1. Start with the last k/v pair.
        cursor.reset()
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

        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, -1)

        # After an error, we can no longer access the key or value
        self.assertCursorHasNoKeyValue(cursor)

        # 2. Setting reset() should place us just after last pair.
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)
            
        prevret = cursor.prev()
        i = self.nentries - 1
        while prevret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i -= 1
            prevret = cursor.prev()

        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, -1)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
