#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# test_util12.py
#	Utilities: wt write
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util12(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util12.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        cursor.set_key('SOMEKEY')
        cursor.set_value('SOMEVALUE')
        cursor.close()

    def test_write(self):
        self.session.create('table:' + self.tablename, self.session_params)
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'])
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'abc')
        self.assertEqual(cursor.get_value(), '123')
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'def')
        self.assertEqual(cursor.get_value(), '456')
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_write_no_keys(self):
        """
        Test write in a 'wt' process, with no args
        """
        self.session.create('table:' + self.tablename, self.session_params)

        errfile = 'writeerr.txt'
        self.runWt(['write', 'table:' + self.tablename], errfilename=errfile)
        self.check_file_contains(errfile, 'usage:')

    def test_write_overwrite(self):
        self.session.create('table:' + self.tablename, self.session_params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        cursor.set_key('def')
        cursor.set_value('789')
        cursor.close()
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'])
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'abc')
        self.assertEqual(cursor.get_value(), '123')
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'def')
        self.assertEqual(cursor.get_value(), '456')
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_write_bad_args(self):
        self.session.create('table:' + self.tablename, self.session_params)
        errfile = 'writeerr.txt'
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc'], errfilename=errfile)
        self.check_file_contains(errfile, 'usage:')


if __name__ == '__main__':
    wttest.run()
