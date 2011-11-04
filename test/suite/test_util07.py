#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util07.py
# 	Utilities: wt read
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util07(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util07.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for i in range(0, self.nentries):
            key = 'KEY' + str(i)
            val = 'VAL' + str(i)
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()
        cursor.close()

    def close_conn(self):
        """
        Close the connection if already open.
        """
        if self.conn != None:
            self.conn.close(None)
            self.conn = None

    def open_conn(self):
        """
        Open the connection if already closed.
        """
        if self.conn == None:
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)

    def test_read_empty(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        outfile = "readout.txt"
        errfile = "readerr.txt"
        self.runWt(["read", 'table:' + self.tablename, 'NoMatch'], outfilename=outfile, errfilename=errfile)
        self.check_empty_file(outfile)
        self.check_file_content(errfile, 'wt: NoMatch: not found\n')

    def test_read_populated(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        outfile = "readout.txt"
        errfile = "readerr.txt"
        self.runWt(["read", 'table:' + self.tablename, 'KEY49'], outfilename=outfile, errfilename=errfile)
        self.check_file_content(outfile, 'VAL49\n')
        self.check_empty_file(errfile)
        self.runWt(["read", 'table:' + self.tablename, 'key49'], outfilename=outfile, errfilename=errfile)
        self.check_empty_file(outfile)
        self.check_file_content(errfile, 'wt: key49: not found\n')


if __name__ == '__main__':
    wttest.run()
