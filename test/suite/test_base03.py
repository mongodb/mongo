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

class test_base03(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_base03a'
    table_name2 = 'test_base03b'
    table_name3 = 'test_base03c'
    table_name4 = 'test_base03d'
    nentries = 10

    def create_table(self, tablename):
        extra_params = ',intl_node_min=512,intl_node_max=16384,leaf_node_min=131072,leaf_node_max=131072'
        self.pr('create_table')
        self.session.create(tablename, 'key_format=S,value_format=S' + extra_params)

    def test_table_ss(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        self.session.create("table:" + self.table_name1, 'key_format=S,value_format=S')
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value('value' + str(i))
            cursor.insert()

        i = 0
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
        self.session.create("table:" + self.table_name2, 'key_format=S,value_format=i')
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name2, None, None)
        for i in range(0, self.nentries):
            cursor.set_key('key' + str(i))
            cursor.set_value(i)
            cursor.insert()

        i = 0
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
        self.session.create("table:" + self.table_name3, 'key_format=i,value_format=S')
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name3, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(i)
            cursor.set_value('value' + str(i))
            cursor.insert()

        i = 0
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
        self.session.create("table:" + self.table_name4, 'key_format=i,value_format=i')
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name4, None, None)
        self.pr('stepping')
        for i in range(0, self.nentries):
            self.pr('put %d -> %d' % (i, i))
            cursor.set_key(i)
            cursor.set_value(i)
            cursor.insert()

        i = 0
        for key, value in cursor:
            self.pr('got %d -> %d' % (key, value))
            self.assertEqual(key, i)
            self.assertEqual(value, i)
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close(None)


if __name__ == '__main__':
    wttest.run()
