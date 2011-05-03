#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test001.py
# 	Basic operations
#

import unittest
import wiredtiger
import wttest

class test001(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test001a.wt'
    table_name2 = 'test001b.wt'

    def create_table(self, tablename):
        extra_params = ',intl_node_min=512,intl_node_max=16384,leaf_node_min=131072,leaf_node_max=131072'
        self.pr('create_table')
        self.session.create('table:' + tablename, 'key_format=S,value_format=S' + extra_params)

    def cursor_s(self, tablename, key):
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        cursor.set_key(key)
        return cursor

    def cursor_ss(self, tablename, key, val):
        cursor = self.cursor_s(tablename, key)
        cursor.set_value(val)
        return cursor

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
        cursor.close(None)

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
        getcursor.close(None)


if __name__ == '__main__':
    wttest.run(test001)
