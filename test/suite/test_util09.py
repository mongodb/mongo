#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util09.py
# 	Utilities: wt loadtext
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util09(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util09.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'

    def populate_file(self, filename, low, high):
        """
        Insert some simple key / value lines into the file
        """
        keys = {}
        with open("loadtext.in", "w") as f:
            for i in range(low, high):
                key = str(i) + str(i)
                val = key + key + key
                f.write(key + '\n')
                f.write(val + '\n')
                keys[key] = val
        #print 'Populated ' + str(len(keys))
        return keys

    def check_keys(self, tablename, keys):
        """
        Check that all the values in the table match the saved dictionary.
        Values in the dictionary are removed as a side effect.
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for key, val in cursor:
            self.assertEqual(keys[key], val)
            del keys[key]
        cursor.close()
        self.assertEqual(len(keys), 0)

    def test_loadtext_empty(self):
        """
        Test loadtext in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 0, 0)
        self.runWt(["loadtext", "-f", "loadtext.in", "table:" + self.tablename])
        self.check_keys(self.tablename, keys)

    def test_loadtext_empty_stdin(self):
        """
        Test loadtext in a 'wt' process using stdin, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 0, 0)
        self.runWt(["loadtext", "table:" + self.tablename], infilename="loadtext.in")
        self.check_keys(self.tablename, keys)

    def test_loadtext_populated(self):
        """
        Test loadtext in a 'wt' process, creating entries in a table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 1010, 1220)
        self.runWt(["loadtext", "-f", "loadtext.in", "table:" + self.tablename])
        self.check_keys(self.tablename, keys)

    def test_loadtext_populated_stdin(self):
        """
        Test loadtext in a 'wt' process using stding, creating entries in a table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 200, 300)
        self.runWt(["loadtext", "table:" + self.tablename], infilename="loadtext.in")
        self.check_keys(self.tablename, keys)


if __name__ == '__main__':
    wttest.run()
