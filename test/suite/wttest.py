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
import traceback
import time

def removeAll(top):
	for root, dirs, files in os.walk(top, topdown=False):
		for name in files:
			os.remove(os.path.join(root, name))
		for name in dirs:
			os.rmdir(os.path.join(root, name))

class WiredTigerTestCase(unittest.TestCase):
    _timeStamp = time.strftime('%Y%m%d-%H%M%S', time.localtime())
    _parentTestdir = 'WT_TEST.' + _timeStamp
    removeAll(_parentTestdir)
    os.makedirs(_parentTestdir)
    _resultfile = open(_parentTestdir + os.sep + 'results.txt', "w", 0)  # unbuffered
    _preserveFiles = False

    @staticmethod
    def globallyPreserveFiles(val):
        WiredTigerTestCase._preserveFiles = val

    # Can be overridden
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create')
        self.pr(`conn`)
        return conn
        
    # Can be overridden
    def setUpSessionOpen(self, conn):
        return conn.open_session(None)
        
    def setUp(self):
        if not hasattr(self.__class__, 'wt_ntests'):
            self.__class__.wt_ntests = 0
        self.__class__.wt_ntests += 1
        self.testdir = WiredTigerTestCase._parentTestdir + os.sep + self.className() + '.' + str(self.__class__.wt_ntests)
        self.prhead('started in ' + self.testdir, True)
        self.origcwd = os.getcwd()
        removeAll(self.testdir)
        if os.path.exists(self.testdir):
            raise Exception(self.testdir + ": cannot remove directory");
        os.makedirs(self.testdir)
        os.chdir(self.testdir)

        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)

    def tearDown(self):
        self.pr('finishing')
        if self.conn != None:
            self.conn.close(None)
            self.conn = None
        os.chdir(self.origcwd)
        # Clean up unless there's a failure
        excinfo = sys.exc_info()
        if excinfo == (None, None, None):
            if not WiredTigerTestCase._preserveFiles:
                removeAll(self.testdir)
            else:
                self.pr('preserving directory ' + self.testdir)
        else:
            self.pr('FAIL')
            self.prexception(excinfo)
            self.pr('preserving directory ' + self.testdir)
        self.prhead('TEST COMPLETED')

    def pr(self, s):
        """
        print a progress line for testing
        """
        msg = '    ' + self.shortid() + ': ' + s
        WiredTigerTestCase._resultfile.write(msg + '\n')

    def prhead(self, s, *beginning):
        """
        print a header line for testing, something important
        """
        msg = ''
        if len(beginning) > 0:
            msg += '\n'
        msg += '  ' + self.shortid() + ': ' + s
        print(msg)
        WiredTigerTestCase._resultfile.write(msg + '\n')

    def prexception(self, excinfo):
        WiredTigerTestCase._resultfile.write('\n')
        traceback.print_exception(excinfo[0], excinfo[1], excinfo[2], None, WiredTigerTestCase._resultfile)
        WiredTigerTestCase._resultfile.write('\n')

    def shortid(self):
        return self.id().replace("__main__.","")

    def className(self):
        return self.__class__.__name__


def runsuite(suite):
    try:
        return unittest.TextTestRunner(verbosity=2).run(suite)
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print('ERROR: running test: ' + repr(name) + ': ', e)
        raise e

def run(name='__main__'):
    result = runsuite(unittest.TestLoader().loadTestsFromName(name))
    sys.exit(not result.wasSuccessful())
