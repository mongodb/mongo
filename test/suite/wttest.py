#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# WiredTigerTestCase
#   parent class for all test cases
#

# If unittest2 is available, use it in preference to (the old) unittest
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import sys, time, traceback, os, re
import wiredtiger
from contextlib import contextmanager

def removeAll(top):
    if not os.path.isdir(top):
        return
    for root, dirs, files in os.walk(top, topdown=False):
        for name in files:
            os.remove(os.path.join(root, name))
        for name in dirs:
            os.rmdir(os.path.join(root, name))
    os.rmdir(top)

def shortenWithEllipsis(s, maxlen):
    if len(s) > maxlen:
        s = s[0:maxlen-3] + '...'
    return s

class FdWriter(object):
    def __init__(self, fd):
        self.fd = fd

    def write(self, text):
        os.write(self.fd, text)


class CapturedFd(object):
    """
    CapturedFd encapsulates a file descriptor (e.g. 1 or 2) that is diverted
    to a file.  We use this to capture and check the C stdout/stderr.
    Meanwhile we reset Python's sys.stdout, sys.stderr, using duped copies
    of the original 1, 2 fds.  The end result is that Python's sys.stdout
    sys.stderr behave normally (e.g. go to the tty), while the C stdout/stderr
    ends up in a file that we can verify.
    """
    def __init__(self, targetFd, originalDupedFd, filename, desc):
        self.targetFd = targetFd
        self.originalDupedFd = originalDupedFd
        self.filename = filename
        self.desc = desc
        self.expectpos = 0

    def readFileFrom(self, filename, pos, maxchars):
        """
        Read a file starting at a given position,
        returning the beginning of its contents
        """
        with open(filename, 'r') as f:
            f.seek(pos)
            return shortenWithEllipsis(f.read(maxchars+1), maxchars)

    def capture(self):
        """
        Start capturing the file descriptor.
        Note that the original targetFd is closed, we expect
        that the caller has duped it and passed the dup to us
        in the constructor.
        """
        filefd = os.open(self.filename, os.O_RDWR | os.O_CREAT | os.O_APPEND)
        os.close(self.targetFd)
        os.dup2(filefd, self.targetFd)
        os.close(filefd)

    def release(self):
        """
        Stop capturing.  Restore the original fd from the duped copy.
        """
        os.close(self.targetFd)
        os.dup2(self.originalDupedFd, self.targetFd)

    def show(self, pfx=None):
        contents = self.readFileFrom(self.filename, 0, 1000)
        if pfx != None:
            pfx = ': ' + pfx
        else:
            pfx = ''
        print self.desc + pfx + ' [pos=' + str(self.expectpos) + '] ' + contents

    def check(self, testcase):
        """
        Check to see that there is no unexpected output in the captured output
        file.  If there is, raise it as a test failure.
        This is generally called after 'release' is called.
        """
        filesize = os.path.getsize(self.filename)
        if filesize > self.expectpos:
            contents = self.readFileFrom(self.filename, self.expectpos, 10000)
            print 'ERROR: ' + self.filename + ' unexpected ' + \
                self.desc + ', contains:\n"' + contents + '"'
            testcase.fail('unexpected ' + self.desc + ', contains: "' +
                      shortenWithEllipsis(contents,100) + '"')

    def checkAdditional(self, testcase, expect):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        gotstr = self.readFileFrom(self.filename, self.expectpos, 1000)
        testcase.assertEqual(gotstr, expect, 'in ' + self.desc +
                             ', expected "' + expect + '", but got "' +
                             gotstr + '"')
        self.expectpos = os.path.getsize(self.filename)

    def checkAdditionalPattern(self, testcase, pat):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        gotstr = self.readFileFrom(self.filename, self.expectpos, 1000)
        if re.search(pat, gotstr) == None:
            testcase.fail('in ' + self.desc +
                          ', expected pattern "' + pat + '", but got "' +
                          gotstr + '"')
        self.expectpos = os.path.getsize(self.filename)


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
        WiredTigerTestCase._stdout = sys.stdout
        WiredTigerTestCase._stderr = sys.stderr
        # newoutfd, newerrfd are dups of the originals
        WiredTigerTestCase._newoutFd = os.dup(1)
        WiredTigerTestCase._newerrFd = os.dup(2)
        # newout, newerr are like stdout, stderr, but using the dups
        WiredTigerTestCase._newout = FdWriter(WiredTigerTestCase._newoutFd)
        WiredTigerTestCase._newerr = FdWriter(WiredTigerTestCase._newerrFd)

    def fdSetUp(self):
        self.captureout = CapturedFd(1, WiredTigerTestCase._newoutFd,
                                     'stdout.txt', 'standard output')
        self.captureerr = CapturedFd(2, WiredTigerTestCase._newerrFd,
                                     'stderr.txt', 'error output')
        self.captureout.capture()
        self.captureerr.capture()
        sys.stdout = WiredTigerTestCase._newout
        sys.stderr = WiredTigerTestCase._newerr
        
    def fdTearDown(self):
        # restore stderr/stdout
        self.captureout.release()
        self.captureerr.release()
        sys.stdout = WiredTigerTestCase._stdout
        sys.stderr = WiredTigerTestCase._stderr
        
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
            self.conn.close()
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
            raise Exception(self.testdir + ": cannot remove directory")
        os.makedirs(self.testdir)
        try:
            os.chdir(self.testdir)
            self.fdSetUp()
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)
        except:
            os.chdir(self.origcwd)
            raise

    def tearDown(self):
        excinfo = sys.exc_info()
        passed = (excinfo == (None, None, None))

        try:
            self.fdTearDown()
            self.pr('finishing')
            self.close_conn()
            # Only check for unexpected output if the test passed
            if passed:
                self.captureout.check(self)
                self.captureerr.check(self)
        finally:
            # always get back to original directory
            os.chdir(self.origcwd)

        # Clean up unless there's a failure
        if passed and not WiredTigerTestCase._preserveFiles:
            removeAll(self.testdir)
        else:
            self.pr('preserving directory ' + self.testdir)

        if not passed:
            print "ERROR"
            self.pr('FAIL')
            self.prexception(excinfo)
            self.pr('preserving directory ' + self.testdir)
        if WiredTigerTestCase._verbose > 2:
            self.prhead('TEST COMPLETED')

    @contextmanager
    def expectedStdout(self, expect):
        self.captureout.check(self)
        yield
        self.captureout.checkAdditional(self, expect)

    @contextmanager
    def expectedStderr(self, expect):
        self.captureerr.check(self)
        yield
        self.captureerr.checkAdditional(self, expect)

    @contextmanager
    def expectedStdoutPattern(self, pat):
        self.captureout.check(self)
        yield
        self.captureout.checkAdditionalPattern(self, pat)

    @contextmanager
    def expectedStderrPattern(self, pat):
        self.captureerr.check(self)
        yield
        self.captureerr.checkAdditionalPattern(self, pat)

    def assertRaisesWithMessage(self, exceptionType, expr, message):
        """
        Like TestCase.assertRaises(), but also checks to see
        that a message is printed on stderr.  If message starts
        and ends with a slash, it is considered a pattern that
        must appear in stderr (it need not encompass the entire
        error output).  Otherwise, the message must match verbatim,
        including any trailing newlines.
        """
        if len(message) > 2 and message[0] == '/' and message[-1] == '/':
            with self.expectedStderrPattern(message[1:-1]):
                self.assertRaises(exceptionType, expr)
        else:
            with self.expectedStderr(message):
                self.assertRaises(exceptionType, expr)
            
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
