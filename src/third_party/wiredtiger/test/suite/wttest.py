#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
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

from contextlib import contextmanager
import os, re, shutil, sys, time, traceback
import wtscenario
import wiredtiger

def shortenWithEllipsis(s, maxlen):
    if len(s) > maxlen:
        s = s[0:maxlen-3] + '...'
    return s

class CapturedFd(object):
    """
    CapturedFd encapsulates a file descriptor (e.g. 1 or 2) that is diverted
    to a file.  We use this to capture and check the C stdout/stderr.
    Meanwhile we reset Python's sys.stdout, sys.stderr, using duped copies
    of the original 1, 2 fds.  The end result is that Python's sys.stdout
    sys.stderr behave normally (e.g. go to the tty), while the C stdout/stderr
    ends up in a file that we can verify.
    """
    def __init__(self, filename, desc):
        self.filename = filename
        self.desc = desc
        self.expectpos = 0
        self.file = None

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
        self.file = open(self.filename, 'w')
        return self.file

    def release(self):
        """
        Stop capturing.
        """
        self.file.close()
        self.file = None

    def check(self, testcase):
        """
        Check to see that there is no unexpected output in the captured output
        file.  If there is, raise it as a test failure.
        This is generally called after 'release' is called.
        """
        if self.file != None:
            self.file.flush()
        filesize = os.path.getsize(self.filename)
        if filesize > self.expectpos:
            contents = self.readFileFrom(self.filename, self.expectpos, 10000)
            WiredTigerTestCase.prout('ERROR: ' + self.filename +
                                     ' unexpected ' + self.desc +
                                     ', contains:\n"' + contents + '"')
            testcase.fail('unexpected ' + self.desc + ', contains: "' +
                      shortenWithEllipsis(contents,100) + '"')
        self.expectpos = filesize

    def checkAdditional(self, testcase, expect):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        if self.file != None:
            self.file.flush()
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
        if self.file != None:
            self.file.flush()
        gotstr = self.readFileFrom(self.filename, self.expectpos, 1000)
        if re.search(pat, gotstr) == None:
            testcase.fail('in ' + self.desc +
                          ', expected pattern "' + pat + '", but got "' +
                          gotstr + '"')
        self.expectpos = os.path.getsize(self.filename)


class TestSuiteConnection(object):
    def __init__(self, conn, connlist):
        connlist.append(conn)
        self._conn = conn
        self._connlist = connlist

    def close(self):
        self._connlist.remove(self._conn)
        return self._conn.close()

    # Proxy everything except what we explicitly define to the
    # wrapped connection
    def __getattr__(self, attr):
        if attr in self.__dict__:
            return getattr(self, attr)
        else:
            return getattr(self._conn, attr)


class WiredTigerTestCase(unittest.TestCase):
    _globalSetup = False
    _printOnceSeen = {}

    # conn_config can be overridden to add to basic connection configuration.
    # Can be a string or a callable function or lambda expression.
    conn_config = ''

    @staticmethod
    def globalSetup(preserveFiles = False, useTimestamp = False,
                    gdbSub = False, verbose = 1, dirarg = None,
                    longtest = False):
        WiredTigerTestCase._preserveFiles = preserveFiles
        d = 'WT_TEST' if dirarg == None else dirarg
        if useTimestamp:
            d += '.' + time.strftime('%Y%m%d-%H%M%S', time.localtime())
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        wtscenario.set_long_run(longtest)
        WiredTigerTestCase._parentTestdir = d
        WiredTigerTestCase._origcwd = os.getcwd()
        WiredTigerTestCase._resultfile = open(os.path.join(d, 'results.txt'), "w", 0)  # unbuffered
        WiredTigerTestCase._gdbSubprocess = gdbSub
        WiredTigerTestCase._longtest = longtest
        WiredTigerTestCase._verbose = verbose
        WiredTigerTestCase._dupout = os.dup(sys.stdout.fileno())
        WiredTigerTestCase._stdout = sys.stdout
        WiredTigerTestCase._stderr = sys.stderr
        WiredTigerTestCase._concurrent = False
        WiredTigerTestCase._globalSetup = True
        WiredTigerTestCase._ttyDescriptor = None

    def fdSetUp(self):
        self.captureout = CapturedFd('stdout.txt', 'standard output')
        self.captureerr = CapturedFd('stderr.txt', 'error output')
        sys.stdout = self.captureout.capture()
        sys.stderr = self.captureerr.capture()

    def fdTearDown(self):
        # restore stderr/stdout
        self.captureout.release()
        self.captureerr.release()
        sys.stdout = WiredTigerTestCase._stdout
        sys.stderr = WiredTigerTestCase._stderr

    def __init__(self, *args, **kwargs):
        if hasattr(self, 'scenarios'):
            assert(len(self.scenarios) == len(dict(self.scenarios)))
        unittest.TestCase.__init__(self, *args, **kwargs)
        if not self._globalSetup:
            WiredTigerTestCase.globalSetup()

    def __str__(self):
        # when running with scenarios, if the number_scenarios() method
        # is used, then each scenario is given a number, which can
        # help distinguish tests.
        scen = ''
        if hasattr(self, 'scenario_number') and hasattr(self, 'scenario_name'):
            scen = '(scenario ' + str(self.scenario_number) + \
                   ': ' + self.scenario_name + ')'
        return self.simpleName() + scen

    def simpleName(self):
        return "%s.%s.%s" %  (self.__module__,
                              self.className(), self._testMethodName)

    # Can be overridden, but first consider setting self.conn_config .
    def setUpConnectionOpen(self, home):
        self.home = home
        config = self.conn_config
        if hasattr(config, '__call__'):
            config = config(home)
        # In case the open starts additional threads, flush first to
        # avoid confusion.
        sys.stdout.flush()
        conn_param = 'create,error_prefix="%s: ",%s' % (self.shortid(), config)
        try:
            conn = self.wiredtiger_open(home, conn_param)
        except wiredtiger.WiredTigerError as e:
            print "Failed wiredtiger_open: dir '%s', config '%s'" % \
                (home, conn_param)
            raise e
        self.pr(`conn`)
        return conn

    # Replacement for wiredtiger.wiredtiger_open that returns
    # a proxied connection that knows to close it itself at the
    # end of the run, unless it was already closed.
    def wiredtiger_open(self, home=None, config=''):
        conn = wiredtiger.wiredtiger_open(home, config)
        return TestSuiteConnection(conn, self._connections)

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

    def open_conn(self, directory="."):
        """
        Open the connection if already closed.
        """
        if self.conn == None:
            self.conn = self.setUpConnectionOpen(directory)
            self.session = self.setUpSessionOpen(self.conn)

    def reopen_conn(self, directory="."):
        """
        Reopen the connection.
        """
        self.close_conn()
        self.open_conn(directory)

    def setUp(self):
        if not hasattr(self.__class__, 'wt_ntests'):
            self.__class__.wt_ntests = 0
        if WiredTigerTestCase._concurrent:
            self.testsubdir = self.shortid() + '.' + str(self.__class__.wt_ntests)
        else:
            self.testsubdir = self.className() + '.' + str(self.__class__.wt_ntests)
        self.testdir = os.path.join(WiredTigerTestCase._parentTestdir, self.testsubdir)
        self.__class__.wt_ntests += 1
        if WiredTigerTestCase._verbose > 2:
            self.prhead('started in ' + self.testdir, True)
        # tearDown needs connections list, set it here in case the open fails.
        self._connections = []
        self.origcwd = os.getcwd()
        shutil.rmtree(self.testdir, ignore_errors=True)
        if os.path.exists(self.testdir):
            raise Exception(self.testdir + ": cannot remove directory")
        os.makedirs(self.testdir)
        os.chdir(self.testdir)
        self.fdSetUp()
        # tearDown needs a conn field, set it here in case the open fails.
        self.conn = None
        try:
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)
        except:
            self.tearDown()
            raise

    def tearDown(self):
        excinfo = sys.exc_info()
        passed = (excinfo == (None, None, None))
        if passed:
            skipped = False
        else:
            skipped = (excinfo[0] == unittest.SkipTest)
        self.pr('finishing')

        # Close all connections that weren't explicitly closed.
        # Connections left open (as a result of a test failure)
        # can result in cascading errors.  We also make sure
        # self.conn is on the list of active connections.
        if not self.conn in self._connections:
            self._connections.append(self.conn)
        for conn in self._connections:
            try:
                conn.close()
            except:
                pass
        self._connections = []

        try:
            self.fdTearDown()
            # Only check for unexpected output if the test passed
            if passed:
                self.captureout.check(self)
                self.captureerr.check(self)
        finally:
            # always get back to original directory
            os.chdir(self.origcwd)

        # Make sure no read-only files or directories were left behind
        os.chmod(self.testdir, 0777)
        for root, dirs, files in os.walk(self.testdir):
            for d in dirs:
                os.chmod(os.path.join(root, d), 0777)
            for f in files:
                os.chmod(os.path.join(root, f), 0666)

        # Clean up unless there's a failure
        if (passed or skipped) and not WiredTigerTestCase._preserveFiles:
            shutil.rmtree(self.testdir, ignore_errors=True)
        else:
            self.pr('preserving directory ' + self.testdir)

        if not passed and not skipped:
            print "ERROR in " + str(self)
            self.pr('FAIL')
            self.prexception(excinfo)
            self.pr('preserving directory ' + self.testdir)
        if WiredTigerTestCase._verbose > 2:
            self.prhead('TEST COMPLETED')

    def backup(self, backup_dir, session=None):
        if session is None:
            session = self.session
        shutil.rmtree(backup_dir, ignore_errors=True)
        os.mkdir(backup_dir)
        bkp_cursor = session.open_cursor('backup:', None, None)
        while True:
            ret = bkp_cursor.next()
            if ret != 0:
                break
            shutil.copy(bkp_cursor.get_key(), backup_dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkp_cursor.close()

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

    def exceptionToStderr(self, expr):
        """
        Used by assertRaisesHavingMessage to convert an expression
        that throws an error to an expression that throws the
        same error but also has the exception string on stderr.
        """
        try:
            expr()
        except BaseException, err:
            sys.stderr.write('Exception: ' + str(err))
            raise

    def assertRaisesHavingMessage(self, exceptionType, expr, message):
        """
        Like TestCase.assertRaises(), but also checks to see
        that the assert exception, when string-ified, includes a message.
        If message starts and ends with a slash, it is considered a pattern that
        must appear (it need not encompass the entire message).
        Otherwise, the message must match verbatim.
        """
        self.assertRaisesWithMessage(
            exceptionType, lambda: self.exceptionToStderr(expr), message)

    @staticmethod
    def printOnce(msg):
        # There's a race condition with multiple threads,
        # but we won't worry about it.  We err on the side
        # of printing the message too many times.
        if not msg in WiredTigerTestCase._printOnceSeen:
            WiredTigerTestCase._printOnceSeen[msg] = msg
            WiredTigerTestCase.prout(msg)

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
            WiredTigerTestCase.prout(message)

    def verbose(self, level, message):
        WiredTigerTestCase.printVerbose(level, message)

    def prout(self, s):
        WiredTigerTestCase.prout(s)

    @staticmethod
    def prout(s):
        os.write(WiredTigerTestCase._dupout, s + '\n')

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
        self.prout(msg)
        WiredTigerTestCase._resultfile.write(msg + '\n')

    def prexception(self, excinfo):
        WiredTigerTestCase._resultfile.write('\n')
        traceback.print_exception(excinfo[0], excinfo[1], excinfo[2], None, WiredTigerTestCase._resultfile)
        WiredTigerTestCase._resultfile.write('\n')

    # print directly to tty, useful for debugging
    def tty(self, message):
        WiredTigerTestCase.tty(message)

    @staticmethod
    def tty(message):
        if WiredTigerTestCase._ttyDescriptor == None:
            WiredTigerTestCase._ttyDescriptor = open('/dev/tty', 'w')
        WiredTigerTestCase._ttyDescriptor.write(message + '\n')

    def ttyVerbose(self, level, message):
        WiredTigerTestCase.ttyVerbose(level, message)

    @staticmethod
    def ttyVerbose(level, message):
        if level <= WiredTigerTestCase._verbose:
            WiredTigerTestCase.tty(message)

    def shortid(self):
        return self.id().replace("__main__.","")

    def className(self):
        return self.__class__.__name__


def longtest(description):
    """
    Used as a function decorator, for example, @wttest.longtest("description").
    The decorator indicates that this test function should only be included
    when running the test suite with the --long option.
    """
    def runit_decorator(func):
        return func
    if not WiredTigerTestCase._longtest:
        return unittest.skip(description + ' (enable with --long)')
    else:
        return runit_decorator

def islongtest():
    return WiredTigerTestCase._longtest

def runsuite(suite, parallel):
    suite_to_run = suite
    if parallel > 1:
        from concurrencytest import ConcurrentTestSuite, fork_for_tests
        if not WiredTigerTestCase._globalSetup:
            WiredTigerTestCase.globalSetup()
        WiredTigerTestCase._concurrent = True
        suite_to_run = ConcurrentTestSuite(suite, fork_for_tests(parallel))
    try:
        return unittest.TextTestRunner(
            verbosity=WiredTigerTestCase._verbose).run(suite_to_run)
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print('ERROR: running test: ', e)
        raise e

def run(name='__main__'):
    result = runsuite(unittest.TestLoader().loadTestsFromName(name), False)
    sys.exit(0 if result.wasSuccessful() else 1)
