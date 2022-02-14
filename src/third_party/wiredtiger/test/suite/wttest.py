#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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
# [TEST_TAGS]
# ignored_file
# [END_TAGS]
#
# WiredTigerTestCase
#   parent class for all test cases
#
from __future__ import print_function

# If unittest2 is available, use it in preference to (the old) unittest
try:
    import unittest2 as unittest
except ImportError:
    import unittest

from contextlib import contextmanager
import errno, glob, os, re, shutil, sys, time, traceback
import wiredtiger, wtscenario, wthooks

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

    def hasUnexpectedOutput(self, testcase):
        """
        Check to see that there is no unexpected output in the captured output
        file.
        """
        if WiredTigerTestCase._ignoreStdout:
            return
        if self.file != None:
            self.file.flush()
        return self.expectpos < os.path.getsize(self.filename)

    def check(self, testcase):
        """
        Check to see that there is no unexpected output in the captured output
        file.  If there is, raise it as a test failure.
        This is generally called after 'release' is called.
        """
        if self.hasUnexpectedOutput(testcase):
            contents = self.readFileFrom(self.filename, self.expectpos, 10000)
            WiredTigerTestCase.prout('ERROR: ' + self.filename +
                                     ' unexpected ' + self.desc +
                                     ', contains:\n"' + contents + '"')
            testcase.fail('unexpected ' + self.desc + ', contains: "' +
                      contents + '"')
        self.expectpos = os.path.getsize(self.filename)

    def ignorePreviousOutput(self):
        """
        Ignore any output up to this point.
        """
        if self.file != None:
            self.file.flush()
        self.expectpos = os.path.getsize(self.filename)

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

    def checkAdditionalPattern(self, testcase, pat, re_flags = 0):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        if self.file != None:
            self.file.flush()
        gotstr = self.readFileFrom(self.filename, self.expectpos, 1500)
        if re.search(pat, gotstr, re_flags) == None:
            testcase.fail('in ' + self.desc +
                          ', expected pattern "' + pat + '", but got "' +
                          gotstr + '"')
        self.expectpos = os.path.getsize(self.filename)

class TestSuiteConnection(object):
    def __init__(self, conn, connlist):
        connlist.append(conn)
        self._conn = conn
        self._connlist = connlist

    def close(self, config=''):
        self._connlist.remove(self._conn)
        return self._conn.close(config)

    # Proxy everything except what we explicitly define to the
    # wrapped connection
    def __getattr__(self, attr):
        if attr in self.__dict__:
            return getattr(self, attr)
        else:
            return getattr(self._conn, attr)

# Just like a list of strings, but with a convenience function
class ExtensionList(list):
    skipIfMissing = False
    def extension(self, dirname, name, extarg=None):
        if name and name != 'none':
            ext = '' if extarg == None else '=' + extarg
            self.append(dirname + '/' + name + ext)

class WiredTigerTestCase(unittest.TestCase):
    _globalSetup = False
    _printOnceSeen = {}
    _ttyDescriptor = None   # set this early, to allow tty() to be called any time.

    # conn_config can be overridden to add to basic connection configuration.
    # Can be a string or a callable function or lambda expression.
    conn_config = ''

    # session_config can be overridden to add to basic session configuration.
    # Can be a string or a callable function or lambda expression.
    session_config = ''

    # conn_extensions can be overridden to add a list of extensions to load.
    # Each entry is a string (directory and extension name) and optional config.
    # Example:
    #    conn_extensions = ('extractors/csv_extractor',
    #                       'test/fail_fs={allow_writes=100}')
    conn_extensions = ()

    @staticmethod
    def globalSetup(preserveFiles = False, removeAtStart = True, useTimestamp = False,
                    gdbSub = False, lldbSub = False, verbose = 1, builddir = None, dirarg = None,
                    longtest = False, zstdtest = False, ignoreStdout = False, seedw = 0, seedz = 0, hookmgr = None):
        WiredTigerTestCase._preserveFiles = preserveFiles
        d = 'WT_TEST' if dirarg == None else dirarg
        if useTimestamp:
            d += '.' + time.strftime('%Y%m%d-%H%M%S', time.localtime())
        if removeAtStart:
            shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        wtscenario.set_long_run(longtest)
        WiredTigerTestCase._parentTestdir = d
        WiredTigerTestCase._builddir = builddir
        WiredTigerTestCase._origcwd = os.getcwd()
        WiredTigerTestCase._resultfile = open(os.path.join(d, 'results.txt'), "w", 1)  # line buffered
        WiredTigerTestCase._gdbSubprocess = gdbSub
        WiredTigerTestCase._lldbSubprocess = lldbSub
        WiredTigerTestCase._longtest = longtest
        WiredTigerTestCase._zstdtest = zstdtest
        WiredTigerTestCase._verbose = verbose
        WiredTigerTestCase._ignoreStdout = ignoreStdout
        WiredTigerTestCase._dupout = os.dup(sys.stdout.fileno())
        WiredTigerTestCase._stdout = sys.stdout
        WiredTigerTestCase._stderr = sys.stderr
        WiredTigerTestCase._concurrent = False
        WiredTigerTestCase._globalSetup = True
        WiredTigerTestCase._seeds = [521288629, 362436069]
        WiredTigerTestCase._randomseed = False
        if hookmgr == None:
            hookmgr = wthooks.WiredTigerHookManager()
        WiredTigerTestCase._hookmgr = hookmgr
        if seedw != 0 and seedz != 0:
            WiredTigerTestCase._randomseed = True
            WiredTigerTestCase._seeds = [seedw, seedz]

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
        self.skipped = False
        if not self._globalSetup:
            WiredTigerTestCase.globalSetup()

    def __str__(self):
        # when running with scenarios, if the number_scenarios() method
        # is used, then each scenario is given a number, which can
        # help distinguish tests.
        scen = ''
        if hasattr(self, 'scenario_number') and hasattr(self, 'scenario_name'):
            scen = ' -s ' + str(self.scenario_number) + \
                   ' (' + self.scenario_name + ')'
        return self.simpleName() + scen

    def shortDesc(self):
        ret_str = ''
        if hasattr(self, 'scenario_number'):
            ret_str = ' -s ' + str(self.scenario_number)
        return self.simpleName() + ret_str

    def simpleName(self):
        return "%s.%s.%s" %  (self.__module__,
                              self.className(), self._testMethodName)

    def buildDirectory(self):
        return self._builddir

    def skipTest(self, reason):
        self.skipped = True
        super(WiredTigerTestCase, self).skipTest(reason)

    # Construct the expected filename for an extension library and return
    # the name if the file exists.
    @staticmethod
    def findExtension(dirname, libname):
        pat = ''
        # When scanning for the extension library, we need to account for
        # the binary paths produced by libtool and CMake. Libtool will export
        # the library under '.libs'.
        if os.path.exists(os.path.join(WiredTigerTestCase._builddir, 'ext',
            dirname, libname, '.libs')):
            pat = os.path.join(WiredTigerTestCase._builddir, 'ext',
                dirname, libname, '.libs', 'libwiredtiger_*.so')
        else:
            pat = os.path.join(WiredTigerTestCase._builddir, 'ext',
                dirname, libname, 'libwiredtiger_*.so')
        filenames = glob.glob(pat)
        if len(filenames) > 1:
            raise Exception(self.shortid() + ": " + ext +
                ": multiple extensions libraries found matching: " + pat)
        return filenames

    # Return the wiredtiger_open extension argument for
    # any needed shared library.
    def extensionsConfig(self):
        exts = self.conn_extensions
        if hasattr(exts, '__call__'):
            exts = ExtensionList()
            self.conn_extensions(exts)
        result = ''
        extfiles = {}
        skipIfMissing = False
        earlyLoading = ''
        if hasattr(exts, 'skip_if_missing'):
            skipIfMissing = exts.skip_if_missing
        if hasattr(exts, 'early_load_ext') and exts.early_load_ext == True:
            earlyLoading = '=(early_load=true)'
        for ext in exts:
            extconf = ''
            if '=' in ext:
                splits = ext.split('=', 1)
                ext = splits[0]
                extconf = '=' + splits[1]
            splits = ext.split('/')
            if len(splits) != 2:
                raise Exception(self.shortid() +
                    ": " + ext +
                    ": extension is not named <dir>/<name>")
            libname = splits[1]
            dirname = splits[0]
            filenames = self.findExtension(dirname, libname)
            if len(filenames) == 0:
                if skipIfMissing:
                    self.skipTest('extension "' + ext + '" not built')
                    continue
                else:
                    raise Exception(self.shortid() +
                        ": " + ext +
                        ": no extensions library found matching: " + pat)
            complete = '"' + filenames[0] + '"' + extconf
            if ext in extfiles:
                if extfiles[ext] != complete:
                    raise Exception(self.shortid() +
                        ": non-matching extension arguments in " +
                        str(exts))
            else:
                extfiles[ext] = complete
        if len(extfiles) != 0:
            result = ',extensions=[' + ','.join(list(extfiles.values())) + earlyLoading + ']'
        return result

    # Can be overridden, but first consider setting self.conn_config
    # or self.conn_extensions
    def setUpConnectionOpen(self, home):
        self.home = home
        config = self.conn_config
        if hasattr(config, '__call__'):
            config = self.conn_config()
        config += self.extensionsConfig()
        # In case the open starts additional threads, flush first to
        # avoid confusion.
        sys.stdout.flush()
        conn_param = 'create,error_prefix="%s",%s' % (self.shortid(), config)
        return self.wiredtiger_open(home, conn_param)

    # Replacement for wiredtiger.wiredtiger_open that returns
    # a proxied connection that knows to close it itself at the
    # end of the run, unless it was already closed.
    def wiredtiger_open(self, home=None, config=''):
        conn = wiredtiger.wiredtiger_open(home, config)
        return TestSuiteConnection(conn, self._connections)

    # Can be overridden, but first consider setting self.session_config
    def setUpSessionOpen(self, conn):
        config = self.session_config
        if hasattr(config, '__call__'):
            config = self.session_config()
        return conn.open_session(config)

    # Can be overridden
    def close_conn(self, config=''):
        """
        Close the connection if already open.
        """
        if self.conn != None:
            self.conn.close(config)
            self.conn = None

    def open_conn(self, directory=".", config=None):
        """
        Open the connection if already closed.
        """
        if self.conn == None:
            if config != None:
                self._old_config = self.conn_config
                self.conn_config = config
            self.conn = self.setUpConnectionOpen(directory)
            if config != None:
                self.conn_config = self._old_config
            self.session = self.setUpSessionOpen(self.conn)

    def reopen_conn(self, directory=".", config=None):
        """
        Reopen the connection.
        """
        self.close_conn()
        self.open_conn(directory, config)

    def setUp(self):
        if not hasattr(self.__class__, 'wt_ntests'):
            self.__class__.wt_ntests = 0

        # We want to have a unique execution directory name for each test.
        # When a test fails, or with the -p option, we want to preserve the
        # directory.  For the non concurrent case, this is easy, we use the
        # class name (like test_base02) and append a class-specific counter.
        # This is short, somewhat descriptive, and unique.
        #
        # Class-specific counters won't work for the concurrent case. To
        # get uniqueness we use a sanitized "short" identifier.  Despite its name,
        # this is not short (with scenarios at least), but it is descriptive
        # and unique.
        if WiredTigerTestCase._concurrent:
            self.testsubdir = self.sanitized_shortid()
        else:
            self.testsubdir = self.className() + '.' + str(self.__class__.wt_ntests)
        self.testdir = os.path.join(WiredTigerTestCase._parentTestdir, self.testsubdir)
        self.__class__.wt_ntests += 1
        self.starttime = time.time()
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
        with open('testname.txt', 'w+') as namefile:
            namefile.write(str(self) + '\n')
        self.fdSetUp()
        # tearDown needs a conn field, set it here in case the open fails.
        self.conn = None
        try:
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)
        except:
            self.tearDown()
            raise

    # Used as part of tearDown determining if there is an error.
    def list2reason(self, result, fieldname):
        exc_list = getattr(result, fieldname, None)
        if exc_list and exc_list[-1][0] is self:
            return exc_list[-1][1]

    def cleanStderr(self):
        self.captureerr.ignorePreviousOutput()

    def cleanStdout(self):
        self.captureout.ignorePreviousOutput()

    def checkStderr(self):
        self.captureerr.check(self)

    def checkStdout(self):
        self.captureout.check(self)

    def tearDown(self):
        # This approach works for all our support Python versions and
        # is suggested by one of the answers in:
        # https://stackoverflow.com/questions/4414234/getting-pythons-unittest-results-in-a-teardown-method
        # In addition, check to make sure exc_info is "clean", because
        # the ConcurrencyTestSuite in Python2 indicates failures using that.
        if hasattr(self, '_outcome'):  # Python 3.4+
            result = self.defaultTestResult()  # these 2 methods have no side effects
            self._feedErrorsToResult(result, self._outcome.errors)
        else:  # Python 3.2 - 3.3 or 3.0 - 3.1 and 2.7
            result = getattr(self, '_outcomeForDoCleanups', self._resultForDoCleanups)
        error = self.list2reason(result, 'errors')
        failure = self.list2reason(result, 'failures')
        exc_failure = (sys.exc_info() != (None, None, None))

        passed = not error and not failure and not exc_failure

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
            self.captureout.check(self)
            self.captureerr.check(self)
        finally:
            # always get back to original directory
            os.chdir(self.origcwd)

        # Make sure no read-only files or directories were left behind
        os.chmod(self.testdir, 0o777)
        for root, dirs, files in os.walk(self.testdir):
            for d in dirs:
                os.chmod(os.path.join(root, d), 0o777)
            for f in files:
                os.chmod(os.path.join(root, f), 0o666)
        self.pr('passed=' + str(passed))
        self.pr('skipped=' + str(self.skipped))

        # Clean up unless there's a failure
        if (passed and (not WiredTigerTestCase._preserveFiles)) or self.skipped:
            shutil.rmtree(self.testdir, ignore_errors=True)
        else:
            self.pr('preserving directory ' + self.testdir)

        elapsed = time.time() - self.starttime
        if elapsed > 0.001 and WiredTigerTestCase._verbose >= 2:
            print("%s: %.2f seconds" % (str(self), elapsed))
        if (not passed) and (not self.skipped):
            print("ERROR in " + str(self))
            self.pr('FAIL')
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
    def expectedStdoutPattern(self, pat, re_flags=0):
        self.captureout.check(self)
        yield
        self.captureout.checkAdditionalPattern(self, pat, re_flags)

    @contextmanager
    def expectedStderrPattern(self, pat, re_flags=0):
        self.captureerr.check(self)
        yield
        self.captureerr.checkAdditionalPattern(self, pat, re_flags)

    def readStdout(self, maxchars=10000):
        return self.captureout.readFileFrom(self.captureout.filename, self.captureout.expectpos, maxchars)

    def readStderr(self, maxchars=10000):
        return self.captureerr.readFileFrom(self.captureerr.filename, self.captureerr.expectpos, maxchars)

    def ignoreStdoutPatternIfExists(self, pat, re_flags=0):
        if self.captureout.hasUnexpectedOutput(self):
            self.captureout.checkAdditionalPattern(self, pat, re_flags)

    def ignoreStderrPatternIfExists(self, pat, re_flags=0):
        if self.captureerr.hasUnexpectedOutput(self):
            self.captureerr.checkAdditionalPattern(self, pat, re_flags)

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

    def assertRaisesException(self, exceptionType, expr,
        exceptionString=None, optional=False):
        """
        Like TestCase.assertRaises(), with some additional options.
        If the exceptionString argument is used, the exception's string
        must match it, or its pattern if the string starts and ends with
        a slash. If optional is set, then no assertion occurs if the
        exception doesn't occur.
        Returns true if the assertion is raised.
        """
        raised = False
        try:
            expr()
        except BaseException as err:
            self.pr('Exception raised shown as string: "' + \
                    str(err) + '"')
            if not isinstance(err, exceptionType):
                self.fail('Exception of incorrect type raised, got type: ' + \
                    str(type(err)))
            if exceptionString != None:
                # Match either a pattern or an exact string.
                fail = False
                self.pr('Expecting string msg: ' + exceptionString)
                if len(exceptionString) > 2 and \
                  exceptionString[0] == '/' and exceptionString[-1] == '/' :
                      if re.search(exceptionString[1:-1], str(err)) == None:
                        fail = True
                elif exceptionString != str(err):
                        fail = True
                if fail:
                    self.fail('Exception with incorrect string raised, got: "' + \
                        str(err) + '" Expected: ' + exceptionString)
            raised = True
        if not raised and not optional:
            self.fail('no assertion raised')
        return raised

    def raisesBusy(self, expr):
        """
        Execute the expression, returning true if a 'Resource busy' exception
        is raised, returning false if no exception is raised. The actual exception
        message can be different across platforms and therefore we rely on os
        libraries to give use the correct exception message.
        Any other exception raises a test suite failure.
        """
        return self.assertRaisesException(wiredtiger.WiredTigerError, \
            expr, os.strerror(errno.EBUSY), optional=True)

    def assertTimestampsEqual(self, ts1, ts2):
        """
        TestCase.assertEqual() for timestamps
        """
        self.assertEqual(int(ts1, 16), int(ts2, 16))

    def timestamp_str(self, t):
        return '%x' % t

    def exceptionToStderr(self, expr):
        """
        Used by assertRaisesHavingMessage to convert an expression
        that throws an error to an expression that throws the
        same error but also has the exception string on stderr.
        """
        try:
            expr()
        except BaseException as err:
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

    def databaseCorrupted(self, directory = None):
        """
        Mark this test as having a corrupted database by creating a
        DATABASE_CORRUPTED file in the home directory.
        """
        if directory == None:
            directory = self.home
        open(os.path.join(directory, "DATABASE_CORRUPTED"), "a").close()

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
        os.write(WiredTigerTestCase._dupout, str.encode(s + '\n'))

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

    def recno(self, i):
        """
        return a recno key
        """
        return i

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

    def sanitized_shortid(self):
        """
        Return a name that is suitable for creating file system names.
        In particular, names with scenarios look like
        'test_file.test_file.test_funcname(scen1.scen2.scen3)'.
        So transform '(', but remove final ')'.
        """
        name = self.shortid().translate(str.maketrans('($[]/ ','______', ')'))

        # On OS/X, we can get name conflicts if names differ by case. Upper
        # case letters are uncommon in our python class and method names, so
        # we lowercase them and prefix with '@', e.g. "AbC" -> "@ab@c".
        return re.sub(r'[A-Z]', lambda x: '@' + x.group(0).lower(), name)

    def className(self):
        return self.__class__.__name__

def zstdtest(description):
    """
    Used as a function decorator, for example, @wttest.zstdtest("description").
    The decorator indicates that this test function should only be included
    when running the test suite with the --zstd option.
    """
    def runit_decorator(func):
        return func
    if not WiredTigerTestCase._zstdtest:
        return unittest.skip(description + ' (enable with --zstd)')
    else:
        return runit_decorator

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

def getseed():
    return WiredTigerTestCase._seeds

def runsuite(suite, parallel):
    suite_to_run = suite
    if parallel > 1:
        from concurrencytest import ConcurrentTestSuite, fork_for_tests
        if not WiredTigerTestCase._globalSetup:
            WiredTigerTestCase.globalSetup()
        WiredTigerTestCase._concurrent = True
        suite_to_run = ConcurrentTestSuite(suite, fork_for_tests(parallel))
    try:
        if WiredTigerTestCase._randomseed:
            WiredTigerTestCase.prout("Starting test suite with seedw={0} and seedz={1}. Rerun this test with -seed {0}.{1} to get the same randomness"
                .format(str(WiredTigerTestCase._seeds[0]), str(WiredTigerTestCase._seeds[1])))
        return unittest.TextTestRunner(
            verbosity=WiredTigerTestCase._verbose).run(suite_to_run)
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print('ERROR: running test: ', e)
        raise e

def run(name='__main__'):
    result = runsuite(unittest.TestLoader().loadTestsFromName(name), False)
    sys.exit(0 if result.wasSuccessful() else 1)
