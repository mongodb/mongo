#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util02.py
# 	Utilities, like wt [ create | drop ]
#

import unittest
from wiredtiger import WiredTigerError
import wttest
import subprocess
import os

class test_util02(wttest.WiredTigerTestCase):
    tablename = 'test_util02.a'
    nentries = 1000

    def check_empty_file(self, filename):
        """
        Raise an error if the file is not empty
        """
        self.assertEqual(os.path.getsize(filename), 0)

    def test_create_process(self):
        """
        Test create in a 'wt' process
        """

        # we close the connection to guarantee everything is
        # flushed, and that we can open it from another process
        self.conn.close(None)
        self.conn = None
        with open("create.err", "w") as createerr:
            with open("create.out", "w") as createout:
                args = ["../../wt", "create"]
                args.append('table:' + self.tablename)
                proc = subprocess.Popen(args, stdout=createout, stderr=createerr)
                proc.wait()
        self.check_empty_file("create.out")
        self.check_empty_file("create.err")
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        for key,val in cursor:
            self.fail('table should be empty')
        cursor.close()

    def test_drop_process(self):
        """
        Test drop in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)

        # we close the connection to guarantee everything is
        # flushed, and that we can open it from another process
        self.conn.close(None)
        self.conn = None
        self.assertTrue(os.path.exists(self.tablename + ".wt"))
        with open("drop.err", "w") as droperr:
            with open("drop.out", "w") as dropout:
                args = ["../../wt", "drop"]
                args.append('table:' + self.tablename)
                proc = subprocess.Popen(args, stdout=dropout, stderr=droperr)
                proc.wait()
        self.check_empty_file("drop.out")
        self.check_empty_file("drop.err")
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)
        self.assertFalse(os.path.exists(self.tablename + ".wt"))
        self.assertRaises(WiredTigerError, lambda: self.session.open_cursor('table:' + self.tablename, None, None))

if __name__ == '__main__':
    wttest.run()
