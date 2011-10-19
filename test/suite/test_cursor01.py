#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base03.py
# 	Cursor operations
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

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
        print('Expect to see messages: \'requires key/value to be set\'')
        self.assertRaises(WiredTigerError, cursor.get_key)
        self.assertRaises(WiredTigerError, cursor.get_value)
        
    def test_forward_iter(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        cursor = self.create_session_and_cursor()

        # TODO: these should fail regardless of table type
        if self.tablekind == 'row':
            self.assertCursorHasNoKeyValue(cursor)

        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            cursor.set_value(self.genvalue(i))
            cursor.insert()

        # Don't use the builtin 'for ... in cursor',
        # iterate using the basic API.

        # 1. Calling first() should place us on the first k/v pair.
        nextret = cursor.first()
        i = 0
        while nextret == 0:
            key = cursor.get_key()
            value = cursor.get_value()
            #print('want: ' + str(self.genkey(i)) + ' got: ' + str(key))
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1
            nextret = cursor.next()

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
        cursor.close(None)

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

        # 1. Calling last() should place us on the last k/v pair.
        prevret = cursor.last()
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
        cursor.close(None)

if __name__ == '__main__':
    wttest.run()
