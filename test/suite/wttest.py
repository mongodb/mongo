#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# See the file LICENSE for redistribution information.
#
# WiredTigerTestCase
#   parent class for all test cases
#

import unittest
import sys
import os
import wiredtiger
import traceback
import time

def removeAll(top):
    if not os.path.isdir(top):
        return
    for root, dirs, files in os.walk(top, topdown=False):
        for name in files:
            os.remove(os.path.join(root, name))
        for name in dirs:
            os.rmdir(os.path.join(root, name))
    os.rmdir(top)

class WiredTigerTestCase(unittest.TestCase):
    _globalSetup = False
    _printOnceSeen = {}

    @staticmethod
    def globalSetup(preserveFiles = False, useTimestamp = False,
                    gdbSub = False, verbose = 1):
        WiredTigerTestCase._preserveFiles = preserveFiles
        if useTimestamp:
            d = 'WT_TEST.' + time.strftime('%Y%m%d-%H%M%S', time.localtime())
        else:
            d = 'WT_TEST'
        removeAll(d)
        os.makedirs(d)
        WiredTigerTestCase._parentTestdir = d
        WiredTigerTestCase._resultfile = open(os.path.join(d, 'results.txt'), "w", 0)  # unbuffered
        WiredTigerTestCase._gdbSubprocess = gdbSub
        WiredTigerTestCase._verbose = verbose
        WiredTigerTestCase._globalSetup = True
    
    def __init__(self, *args, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        if not self._globalSetup:
            WiredTigerTestCase.globalSetup()

    def __str__(self):
        # when running with scenarios, if the number_scenarios() method
        # is used, then each scenario is given a number, which can
        # help distinguish tests.
        scen = ''
        if hasattr(self, 'scenario_number'):
            scen = '(scenario ' + str(self.scenario_number) + ')'
        return self.simpleName() + scen

    def simpleName(self):
        return "%s.%s.%s" %  (self.__module__,
                              self.className(), self._testMethodName)

    # Can be overridden
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create,error_prefix="' +
                                          self.shortid() + ': ' + '"')
        self.pr(`conn`)
        return conn
        
    # Can be overridden
    def setUpSessionOpen(self, conn):
        return conn.open_session(None)
        
    # Can be overridden
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

    def reopen_conn(self):
        """
        Reopen the connection.
        """
        self.close_conn()
        self.open_conn()

    def setUp(self):
        if not hasattr(self.__class__, 'wt_ntests'):
            self.__class__.wt_ntests = 0
        self.__class__.wt_ntests += 1
        self.testdir = os.path.join(WiredTigerTestCase._parentTestdir, self.className() + '.' + str(self.__class__.wt_ntests))
        if WiredTigerTestCase._verbose > 2:
            self.prhead('started in ' + self.testdir, True)
        self.origcwd = os.getcwd()
        removeAll(self.testdir)
        if os.path.exists(self.testdir):
            raise Exception(self.testdir + ": cannot remove directory");
        os.makedirs(self.testdir)
        try:
            os.chdir(self.testdir)
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)
        except:
            os.chdir(self.origcwd)
            raise

    def tearDown(self):
        self.pr('finishing')
        self.close_conn()
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
        if WiredTigerTestCase._verbose > 2:
            self.prhead('TEST COMPLETED')

    @staticmethod
    def printOnce(msg):
        # There's a race condition with multiple threads,
        # but we won't worry about it.  We err on the side
        # of printing the message too many times.
        if not msg in WiredTigerTestCase._printOnceSeen:
            WiredTigerTestCase._printOnceSeen[msg] = msg
            print msg

    def KNOWN_FAILURE(self, name):
        myname = self.simpleName()
        msg = '**** ' + myname + ' HAS A KNOWN FAILURE: ' + name + ' ****'
        self.printOnce(msg)
        self.skipTest('KNOWN FAILURE: ' + name)

    def KNOWN_LIMITATION(self, name):
        myname = self.simpleName()
        msg = '**** ' + myname + ' HAS A KNOWN LIMITATION: ' + name + ' ****'
        self.printOnce(msg)

    @staticmethod
    def printVerbose(level, message):
        if level <= WiredTigerTestCase._verbose:
            print message

    def verbose(self, level, message):
        WiredTigerTestCase.printVerbose(level, message)

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
        return unittest.TextTestRunner(
            verbosity=WiredTigerTestCase._verbose).run(suite)
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print('ERROR: running test: ', e)
        raise e

def run(name='__main__'):
    result = runsuite(unittest.TestLoader().loadTestsFromName(name))
    sys.exit(not result.wasSuccessful())
