#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util11.py
#	Utilities: wt list
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util11(wttest.WiredTigerTestCase, suite_subprocess):
    tablenamepfx = 'test_util11.'
    session_params = 'key_format=S,value_format=S'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        cursor.set_key('SOMEKEY')
        cursor.set_value('SOMEVALUE')
        cursor.close()

    def test_list_none(self):
        """
        Test list in a 'wt' process, with no tables
        """

        # Construct what we think we'll find
        filelist = ''
        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist)

    def test_list(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables
        """
        pfx = self.tablenamepfx
        params = self.session_params
        self.session.create('table:' + pfx + '5', params)
        self.session.create('table:' + pfx + '3', params)
        self.session.create('table:' + pfx + '1', params)
        self.session.create('table:' + pfx + '2', params)
        self.session.create('table:' + pfx + '4', params)
        self.populate(pfx + '2')
        self.populate(pfx + '3')

        # Construct what we think we'll find
        filelist = 'file:WiredTiger.wt\n'
        tablelist = ''
        for i in range(1, 6):
            filelist += 'file:' + pfx + str(i) + '.wt\n'
            tablelist += 'table:' + pfx + str(i) + '\n'

        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist + tablelist)

    def test_list_drop(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables,
        after some tables have been dropped.
        """
        pfx = self.tablenamepfx
        params = self.session_params
        self.session.create('table:' + pfx + '5', params)
        self.session.create('table:' + pfx + '3', params)
        self.session.create('table:' + pfx + '1', params)
        self.session.create('table:' + pfx + '2', params)
        self.session.create('table:' + pfx + '4', params)
        self.populate(pfx + '2')
        self.populate(pfx + '3')
        self.session.drop('table:' + pfx + '2', None)
        self.session.drop('table:' + pfx + '4', None)

        # Construct what we think we'll find
        filelist = 'file:WiredTiger.wt\n'
        tablelist = ''
        filelist += 'file:' + pfx + '1.wt\n'
        tablelist += 'table:' + pfx + '1\n'
        filelist += 'file:' + pfx + '3.wt\n'
        tablelist += 'table:' + pfx + '3\n'
        filelist += 'file:' + pfx + '5.wt\n'
        tablelist += 'table:' + pfx + '5\n'

        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist + tablelist)

    def test_list_drop_all(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables,
        after all tables have been dropped.
        """
        pfx = self.tablenamepfx
        params = self.session_params
        self.session.create('table:' + pfx + '5', params)
        self.session.create('table:' + pfx + '3', params)
        self.session.create('table:' + pfx + '1', params)
        self.session.create('table:' + pfx + '2', params)
        self.session.create('table:' + pfx + '4', params)
        self.populate(pfx + '2')
        self.populate(pfx + '3')
        self.session.drop('table:' + pfx + '5', None)
        self.session.drop('table:' + pfx + '4', None)
        self.session.drop('table:' + pfx + '3', None)
        self.session.drop('table:' + pfx + '2', None)
        self.session.drop('table:' + pfx + '1', None)

        # Construct what we think we'll find
        filelist = 'file:WiredTiger.wt\n'
        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist)


if __name__ == '__main__':
    wttest.run()
