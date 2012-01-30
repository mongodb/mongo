#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# test_config05.py
# 	Test multiple connection opens
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest
import os

class test_config05(wttest.WiredTigerTestCase):
    table_name1 = 'test_config05'
    nentries = 100

    # Each test needs to set up its connection in its own way,
    # so override these methods to do nothing
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def close_conn(self):
        if self.conn != None:
            self.conn.close(None)
            self.conn = None
        if hasattr(self, 'conn2') and self.conn2 != None:
            self.conn2.close(None)
            self.conn2 = None

    def populate(self, session):
        """
        Create entries using key=string, value=string
        """
        create_args = 'key_format=S,value_format=S'
        session.create("table:" + self.table_name1, create_args)
        cursor = session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(str(1000000 + i))
            cursor.set_value('value' + str(i))
            cursor.insert()
        cursor.close(None)

    def verify_entries(self, session):
        """
        Verify all entries created in populate()
        """
        cursor = session.open_cursor('table:' + self.table_name1, None, None)
        i = 0
        for key, value in cursor:
            self.assertEqual(key, str(1000000 + i))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def test_one(self):
        self.conn = wiredtiger.wiredtiger_open('.', 'create')
        self.session = self.conn.open_session(None)
        self.populate(self.session)
        self.verify_entries(self.session)

    def test_multi_create(self):
        self.conn = wiredtiger.wiredtiger_open('.', 'create')
        self.session = self.conn.open_session(None)
        self.assertRaises(WiredTigerError, lambda: wiredtiger.wiredtiger_open
                          ('.', 'create'))

if __name__ == '__main__':
    wttest.run()
