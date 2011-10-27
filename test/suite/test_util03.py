#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util03.py
# 	Utilities: wt create
#

import unittest
from wiredtiger import WiredTigerError
import wttest
import subprocess
import os

class test_util03(wttest.WiredTigerTestCase):
    tablename = 'test_util03.a'
    nentries = 1000

    scenarios = [
        ('none', dict(key_format=None,value_format=None)),
        ('SS', dict(key_format='S',value_format='S')),
        ('rS', dict(key_format='r',value_format='S')),
        ('ri', dict(key_format='r',value_format='i')),
        ]

    def check_empty_file(self, filename):
        """
        Raise an error if the file is not empty
        """
        filesize = os.path.getsize(filename)
        if filesize > 0:
            with open(filename, 'r') as f:
                contents = f.read(1000)
                print 'ERROR: ' + filename + ' should be empty, but contains:\n'
                print contents + '...\n'
        self.assertEqual(filesize, 0)

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
                #args = ["gdb", "--args", "../../.libs/wt", "create"]
                args = ["../../wt", "create"]
                if self.key_format != None or self.value_format != None:
                    args.append('-c')
                    config = ''
                    if self.key_format != None:
                        config += 'key_format=' + self.key_format + ','
                    if self.value_format != None:
                        config += 'value_format=' + self.value_format
                    args.append(config)
                args.append('table:' + self.tablename)
                proc = subprocess.Popen(args, stdout=createout, stderr=createerr)
                proc.wait()
        self.check_empty_file("create.out")
        self.check_empty_file("create.err")
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        if self.key_format != None:
            self.assertEqual(cursor.key_format, self.key_format)
        if self.value_format != None:
            self.assertEqual(cursor.value_format, self.value_format)
        for key,val in cursor:
            self.fail('table should be empty')
        cursor.close()


if __name__ == '__main__':
    wttest.run()
