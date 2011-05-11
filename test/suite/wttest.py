#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# WiredTigerTestCase
# 	parent class for all test cases
#

import unittest
import sys
import os
import wiredtiger
import subprocess

class WiredTigerTestCase(unittest.TestCase):
    def setUp(self):
        self.prhead('started', True)
        self.testdir = 'WT_TEST'
        self.origcwd = os.getcwd()

        subprocess.call(["rm", "-rf", self.testdir])
        if os.path.exists(self.testdir):
            raise Exception(self.testdir + ": cannot remove directory");
        subprocess.call(["mkdir", self.testdir])
        os.chdir("WT_TEST")

        self.conn = wiredtiger.wiredtiger_open(".", None, 'create')
        self.pr(`self.conn`)
        self.session = self.conn.open_session(None, None)

    def tearDown(self):
        self.pr('finishing')
        self.conn.close(None)
        os.chdir(self.origcwd)
        self.prhead('TEST COMPLETED')

    def pr(self, s):
        """
        print a progress line for testing
        """
        print('    ' + self.shortid() + ': ' + s)

    def prhead(self, s, *beginning):
        """
        print a header line for testing, something important
        """
        if len(beginning) > 0:
            print('')
        print('  ' + self.shortid() + ': ' + s)

    def shortid(self):
        return self.id().replace("__main__.","")


def runsuite(suite):
    try:
        unittest.TextTestRunner(verbosity=2).run(suite)
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print('ERROR: running test: ' + repr(name) + ': ', e)
        raise e

def run(name):
    runsuite(unittest.TestLoader().loadTestsFromTestCase(name))
