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
import wttest
import wtscenario

class test_base03(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_base03a'
    table_name2 = 'test_base03b'
    table_name3 = 'test_base03c'
    table_name4 = 'test_base03d'
    nentries = 10

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

    def test_table_ss(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value('value' + str(i))
            cursor.insert()

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def test_table_si(self):
        """
        Create entries, and read back in a cursor: key=string, value=int
        """
        create_args = 'key_format=S,value_format=i' + self.config_string()
        self.session_create("table:" + self.table_name2, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name2, None, None)
        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value(i)
            cursor.insert()

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got: ' + str(key) + ': ' + str(value))
            self.assertEqual(key, 'key' + str(i))
            self.assertEqual(value, i)
            i += 1

        self.pr("i = " + str(i))
        self.pr("self.... = " + str(self.nentries))
        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def test_table_is(self):
        """
        Create entries, and read back in a cursor: key=int, value=string
        """
        create_args = 'key_format=i,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name3, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name3, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(i)
            cursor.set_value('value' + str(i))
            cursor.insert()

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got: ' + str(key) + ': ' + str(value))
            self.assertEqual(key, i)
            self.assertEqual(value, 'value' + str(i))
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def test_table_ii(self):
        """
        Create entries, and read back in a cursor: key=int, value=int
        """
        create_args = 'key_format=i,value_format=i' + self.config_string()
        self.session_create("table:" + self.table_name4, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name4, None, None)
        self.pr('stepping')
        for i in range(0, self.nentries):
            self.pr('put %d -> %d' % (i, i))
            cursor.set_key(i)
            cursor.set_value(i)
            cursor.insert()

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got %d -> %d' % (key, value))
            self.assertEqual(key, i)
            self.assertEqual(value, i)
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close(None)


if __name__ == '__main__':
    wttest.run()
