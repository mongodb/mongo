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

import unittest

from contextlib import contextmanager
import errno, glob, os, re, shutil, sys, threading, time, traceback, types
import abstract_test_case, test_result, wiredtiger, wthooks, wtscenario

# Use as "with timeout(seconds): ....". Argument of 0 means no timeout,
# and only available (with non-zero argument) on Unix systems.
class timeout(object):
    def __init__(self, seconds=0):
        self.seconds = seconds
        self.prev_handler = None

    def signal_handler(self, signum, frame):
        raise(TimeoutError('time for test exceeded {} seconds'.format(self.seconds)))

    def __enter__(self):
        if self.seconds != 0:
            try:
                import signal     # This will fail on non-Unix systems.
                self.prev_handler = signal.signal(signal.SIGALRM, self.signal_handler)
                signal.alarm(self.seconds)
            except Exception as e:
                raise Exception('The --timeout option is not available on this system: ' + str(e))

    def __exit__(self, typ, value, traceback):
        if self.seconds != 0:
            try:
                import signal     # This will fail on non-Unix systems.
                signal.alarm(0)
                signal.signal(signal.SIGALRM, self.prev_handler)
            except Exception as e:
                raise Exception('The --timeout option is not available on this system: ' + str(e))

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

class WiredTigerTestCase(abstract_test_case.AbstractWiredTigerTestCase):
    _globalSetup = False

    # We store the current test case in thread local storage.  There are
    # certain odd cases where this is useful, like hooks, where we don't
    # have any notion of the current test case.
    _threadLocal = threading.local()

    # rollbacks_allowed can be overridden to permit more or fewer retries on rollback errors.
    # We retry tests that get rollback errors in a way that is mostly invisible.
    # There is a visible difference in that the rollback error's stack trace is recorded
    # in the results.txt .  If a single test fails more than self.rollbacks_allowed,
    # we raise an error, and the test fails.
    rollbacks_allowed = 2

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
                    longtest = False, extralongtest = False, zstdtest = False, ignoreStdout = False,
                    seedw = 0, seedz = 0, hookmgr = None, ss_random_prefix = 0, timeout = 0):
        parentTestDir = 'WT_TEST' if dirarg == None else dirarg
        wtscenario.set_long_run(longtest)
        WiredTigerTestCase._builddir = builddir
        WiredTigerTestCase._gdbSubprocess = gdbSub
        WiredTigerTestCase._lldbSubprocess = lldbSub
        WiredTigerTestCase._longtest = longtest
        WiredTigerTestCase._extralongtest = extralongtest
        WiredTigerTestCase._zstdtest = zstdtest
        WiredTigerTestCase._concurrent = False
        WiredTigerTestCase._ss_random_prefix = ss_random_prefix
        WiredTigerTestCase._retriesAfterRollback = 0
        WiredTigerTestCase._testsRun = 0
        WiredTigerTestCase._timeout = timeout
        if hookmgr == None:
            hookmgr = wthooks.WiredTigerHookManager()
        WiredTigerTestCase._hookmgr = hookmgr
        WiredTigerTestCase.hook_names = hookmgr.get_hook_names()

        WiredTigerTestCase.setupTestDir(parentTestDir, preserveFiles, removeAtStart, useTimestamp)
        WiredTigerTestCase.setupIO('results.txt', ignoreStdout, verbose)
        WiredTigerTestCase.setupRandom(seedw, seedz)
        WiredTigerTestCase._globalSetup = True

    @staticmethod
    def finalReport():
        # We retry tests that get rollback errors in a way that is mostly invisible.
        # self.rollbacks_allowed makes sure a single test doesn't rollback too many times.
        # We also want to report an error if the total number of retries in the entire run gets
        # above a threshold, we use 1%.  To prevent a shorter run (say 50 tests) from tripping
        # over this if it happens to have 1 or 2 hits, we won't fail unless there's an absolute
        # quantity of failures above 3.
        totalTestsRun = WiredTigerTestCase._testsRun
        totalRetries = WiredTigerTestCase._retriesAfterRollback
        if totalTestsRun > 0 and totalRetries / totalTestsRun > 0.01 and totalRetries >= 3:
            raise Exception('Retries from WT_ROLLBACK in test suite: {}/{}, see {} for stack traces'.format(
                totalRetries, totalTestsRun, WiredTigerTestCase._resultFileName))

    @staticmethod
    def currentTestCase():
        return getattr(WiredTigerTestCase._threadLocal, 'currentTestCase', None)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.skipped = False
        self.teardown_actions = []
        if not self._globalSetup:
            WiredTigerTestCase.globalSetup()
        self.platform_api = WiredTigerTestCase._hookmgr.get_platform_api()

    # Platform specific functions (may be overridden by hooks):

    # Return true if file(s) for the table with the given base name exist in the file system.
    # This may have a different implementation when running under certain hooks.
    def tableExists(self, name):
        return self.platform_api.tableExists(name)

    # The first filename for this URI.  In the tiered storage
    # world, this makes a difference, every flush tier creates a
    # This may have a different implementation when running under certain hooks.
    def initialFileName(self, name):
        return self.platform_api.initialFileName(name)

    # Return the WiredTigerTimestamp for this testcase, or None if there is none.
    def getTimestamp(self):
        return self.platform_api.getTimestamp()

    # Return the tier share percent for this testcase, or 0 if there is none.
    def getTierSharePercent(self):
        return self.platform_api.getTierSharePercent()

    # Return the tier cache percent for this testcase, or 0 if there is none.
    def getTierCachePercent(self):
        return self.platform_api.getTierCachePercent()

    # Return the tier storage source for this testcase, or 'dir_store' if there is none.
    def getTierStorageSource(self):
        return self.platform_api.getTierStorageSource()

    def buildDirectory(self):
        return self._builddir

    def skipTest(self, reason):
        self.skipped = True
        super(WiredTigerTestCase, self).skipTest(reason)

    # Overridden from unittest.  Run the method, but retry rollback errors, up to a point.
    # Note that this method first appears in Python 3.7.  When running with an older Python,
    # this method does not override anything.  The test method is called a different way,
    # and we will not get any retry behavior.
    def _callTestMethod(self, method):
        rollbacksAllowed = self.rollbacks_allowed
        finished = False
        WiredTigerTestCase._testsRun += 1
        while not finished and rollbacksAllowed >= 0:
            try:
                with timeout(WiredTigerTestCase._timeout):
                    method()
                    finished = True
            except wiredtiger.WiredTigerRollbackError:
                WiredTigerTestCase._retriesAfterRollback += 1
                self.prexception(sys.exc_info())
                if rollbacksAllowed == 0:
                    self.pr('rollback error, no more restarts, failing test.')
                    raise
                else:
                    self.pr('rollback error, restarting test.')
                    self.tearDown(True)
                    if WiredTigerTestCase._verbose > 2:
                        print("[pid:{}]: {}: restarting after rollback error".format(os.getpid(), self))
                    self.setUp()
                    rollbacksAllowed -= 1

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

        # Collect all statistics for Python tests by default unless configured otherwise.
        if not("statistics" in config):
            config += ',statistics=(all)'

        # Enable statistics logging if we haven't already.
        if ("statistics=(all)" in config or "statistics=(fast)" in config) and not("statistics_log" in config):
            config += ',statistics_log=(wait=1,json=true,on_close=true)'

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

    @contextmanager
    def transaction(self, session=None, begin_config=None, commit_config=None,
                    commit_timestamp=None, read_timestamp=None, rollback=False):
        if session == None:
            session = self.session
        if commit_timestamp != None:
            if commit_config != None:
                raise Exception('transaction: commit_timestamp and commit_config cannot both be set')
            commit_config = 'commit_timestamp=' + self.timestamp_str(commit_timestamp)
        if read_timestamp != None:
            if begin_config != None or commit_config != None:
                raise Exception('transaction: read_timestamp cannot be set with either ' +
                    'begin_config, commit_config, commit_timestamp')
            begin_config = 'read_timestamp=' + self.timestamp_str(read_timestamp)

        session.begin_transaction(begin_config)
        yield
        if rollback:
            session.rollback_transaction()
        else:
            session.commit_transaction(commit_config)

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
            self.testsubdir = self.class_name() + '.' + str(self.__class__.wt_ntests)
        self.testdir = os.path.join(WiredTigerTestCase._parentTestdir, self.testsubdir)
        self.__class__.wt_ntests += 1
        self.starttime = time.time()
        if WiredTigerTestCase._verbose > 2:
            self.prhead('started in ' + self.testdir, True)
        # tearDown needs connections list, set it here in case the open fails.
        self._connections = []
        self._failed = None   # set to True/False during teardown.

        self.platform_api.setUp()
        self.origcwd = os.getcwd()
        shutil.rmtree(self.testdir, ignore_errors=True)
        if os.path.exists(self.testdir):
            raise Exception(self.testdir + ": cannot remove directory")
        os.makedirs(self.testdir)
        os.chdir(self.testdir)
        with open('testname.txt', 'w+') as namefile:
            namefile.write(str(self) + '\n')
        if WiredTigerTestCase._verbose >= 2:
            print("[pid:{}]: {}: starting".format(os.getpid(), str(self)))
        self.fdSetUp()
        self._threadLocal.currentTestCase = self
        self.ignoreTearDownLogs = False
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

    def ignoreStdoutPattern(self, pattern, re_flags = 0):
        self.ignore_regex = re.compile(pattern, re_flags)

    def readyDirectoryForRemoval(self, directory):
        # Make sure any read-only files or directories left behind
        # are made writeable in preparation for removal.
        os.chmod(directory, 0o777)
        for root, dirs, files in os.walk(directory):
            for d in dirs:
                os.chmod(os.path.join(root, d), 0o777)
            for f in files:
                os.chmod(os.path.join(root, f), 0o666)

    # Return value of each action should be a tuple with the first value an integer (non-zero to indicate
    # failure), and the second value a string suitable for printing when the test fails.
    def addTearDownAction(self, action):
        self.teardown_actions.append(action)

    def tearDown(self, dueToRetry=False):
        teardown_failed = False
        teardown_msg = None
        if not dueToRetry:
            for action in self.teardown_actions:
                try:
                    tmp = action()
                except:
                    e = sys.exc_info()
                    self.prexception(e)
                    tmp = (-1, str(e[1]))
                if tmp[0] != 0:
                    self.pr('ERROR: teardown action failed, message=' + tmp[1])
                    teardown_failed = True
                    if teardown_msg is None:
                        teardown_msg = str(tmp[1])
                    else:
                        teardown_msg += "; " + str(tmp[1])

        passed = not (self.failed() or teardown_failed)

        try:
            self.platform_api.tearDown()
        except:
            self.pr('ERROR: failed to tear down the platform API')
            self.prexception(sys.exc_info())

        # Download the files from the bucket for tiered tests if the test fails or preserve is
        # turned on.
        try:
            if hasattr(self, 'ss_name') and not self.skipped and \
                (not passed or WiredTigerTestCase._preserveFiles):
                    self.pr('downloading object files')
                    self.download_objects(self.bucket, self.bucket_prefix)
        except:
            self.pr('ERROR: failed to download objects')
            self.prexception(sys.exc_info())

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
            if not (dueToRetry or self.ignoreTearDownLogs):
                self.captureout.check(self)
                self.captureerr.check(self)
        finally:
            # always get back to original directory
            os.chdir(self.origcwd)

        self.pr('passed=' + str(passed))
        self.pr('skipped=' + str(self.skipped))

        # Clean up unless there's a failure
        self.readyDirectoryForRemoval(self.testdir)
        if (passed and (not WiredTigerTestCase._preserveFiles)) or self.skipped:
            try:
                shutil.rmtree(self.testdir, ignore_errors=True)
            except:
                self.pr('ERROR: failed to delete the test directory: ' + self.testdir)
                self.prexception(sys.exc_info())
        else:
            self.pr('preserving directory ' + self.testdir)

        self._threadLocal.currentTestCase = None

        elapsed = time.time() - self.starttime
        if elapsed > 0.001 and WiredTigerTestCase._verbose >= 2:
            print("[pid:{}]: {}: {:.2f} seconds".format(os.getpid(), str(self), elapsed))
        if teardown_failed:
            self.fail(f'Teardown of {self} failed with message: {teardown_msg}')
        if (not passed or teardown_failed) and (not self.skipped):
            print("[pid:{}]: ERROR in {}".format(os.getpid(), str(self)))
            self.pr('FAIL')
            self.pr('preserving directory ' + self.testdir)
        if WiredTigerTestCase._verbose > 2:
            self.prhead('TEST COMPLETED')

    # Returns None if testcase is running.  If during (or after) tearDown,
    # will return True or False depending if the test case failed.
    def failed(self):
        return self._failed

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

    def runningHook(self, name):
        return name in WiredTigerTestCase.hook_names

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
    def expectedStdoutPattern(self, pat, re_flags=0, maxchars=1500):
        self.captureout.check(self)
        yield
        self.captureout.checkAdditionalPattern(self, pat, re_flags, maxchars)

    @contextmanager
    def expectedStderrPattern(self, pat, re_flags=0, maxchars=1500):
        self.captureerr.check(self)
        yield
        self.captureerr.checkAdditionalPattern(self, pat, re_flags, maxchars)

    @contextmanager
    def customStdoutPattern(self, f):
        self.captureout.check(self)
        yield
        self.captureout.checkCustomValidator(self, f)

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

    # Some tests do table drops as a means to perform some test repeatedly in a loop.
    # These tests require that a name be completely removed before the next iteration
    # can begin.  However, tiered storage does not always provide a way to remove or
    # rename objects that have been stored to the cloud, as doing that is not the normal
    # part of a workflow (at this writing, GC is not yet implemented). Most storage sources
    # return ENOTSUP when asked to remove a cloud object, so we really don't have a way to
    # clear out the name space, and so we skip these tests under tiered storage.
    #
    # Note: as part of PM-3389, we may end up with unique names for every cloud object.
    # If so, we could remove this restriction.
    def requireDropRemovesNameConflict(self):
        if self.runningHook('tiered'):
            self.skipTest('Test requires removal from cloud storage, which is not yet permitted')

    def compactUntilSuccess(self, session, uri, config=None):
        while True:
            try:
                session.compact(uri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err

    def dropUntilSuccess(self, session, uri, config=None):
        # Most test cases consider a drop, and especially a 'drop until success',
        # to completely remove a file's artifacts, so that the name can be reused.
        # Require this behavior.
        self.requireDropRemovesNameConflict()
        while True:
            try:
                session.drop(uri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err
                session.checkpoint()

    def verifyUntilSuccess(self, session, uri, config=None):
        while True:
            try:
                session.verify(uri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err
                session.checkpoint()

    def renameUntilSuccess(self, session, uri, newUri, config=None):
        while True:
            try:
                session.rename(uri, newUri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err
                session.checkpoint()

    def upgradeUntilSuccess(self, session, uri, config=None):
        while True:
            try:
                session.upgrade(uri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err
                session.checkpoint()

    def salvageUntilSuccess(self, session, uri, config=None):
        while True:
            try:
                session.salvage(uri, config)
                return
            except wiredtiger.WiredTigerError as err:
                if str(err) != os.strerror(errno.EBUSY):
                    raise err
                session.checkpoint()

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

    def databaseCorrupted(self, directory = None):
        """
        Mark this test as having a corrupted database by creating a
        DATABASE_CORRUPTED file in the home directory.
        """
        if directory == None:
            directory = self.home
        open(os.path.join(directory, "DATABASE_CORRUPTED"), "a").close()

    def recno(self, i):
        """
        return a recno key
        """
        return i

@contextmanager
def open_cursor(session, uri: str, **kwargs):
    """
    Open a cursor instance on a session.

    Supports 'with' statements.

    Args:
        uri (str): URI.

    Keyword Args:
        config (str): Configuration.
    """

    config = None if "config" not in kwargs else str(kwargs["config"])

    cursor = session.open_cursor(uri, None, config)
    try:
        yield cursor
    finally:
        cursor.close()


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

def extralongtest(description):
    """
    Used as a function decorator, for example, @wttest.extralongtest("description").
    The decorator indicates that this test function should only be included
    when running the test suite with the --extra-long option.
    """
    def runit_decorator(func):
        return func
    if not WiredTigerTestCase._extralongtest:
        return unittest.skip(description + ' (enable with --extra-long)')
    else:
        return runit_decorator

def prevent(what):
    """
    Used as a function decorator, for example, @wttest.prevent("timestamp").
    The decorator indicates that this test function uses some facility (in the
    example, it uses its own timestamps), and prevents hooks that try to manage
    timestamps from running the test.
    """
    def runit_decorator(func):
        return func
    hooks_using = WiredTigerTestCase._hookmgr.hooks_using(what)
    if len(hooks_using) > 0:
        return unittest.skip("function uses {}, which is incompatible with hooks: {}".format(what, hooks_using))
    else:
        return runit_decorator

def skip_for_hook(hookname, description):
    """
    Used as a function decorator, for example, @wttest.skip_for_hook("tiered", "fails at commit_transaction").
    The decorator indicates that this test function fails with the hook, which should be investigated.
    """
    def runit_decorator(func):
        return func
    if hookname in WiredTigerTestCase.hook_names:
        return unittest.skip("because running with hook '{}': {}".format(hookname, description))
    else:
        return runit_decorator

def islongtest():
    return WiredTigerTestCase._longtest

def getseed():
    return WiredTigerTestCase._seeds

def getss_random_prefix():
    return WiredTigerTestCase._ss_random_prefix

# We have to override the ThreadsafeForwardingResult implementation of tags so it gets set immediately
# which allows us to set the pid of the process on our output stream to make debugging easier.
def immediate_tags(self, new_tags, gone_tags):
    self.result.tags(new_tags, gone_tags)

def wrap_result_for_tags(thread_safe_result, thread_number):
    # We use this technique to override the method instead of extending the class as it allows for less changes.
    thread_safe_result.tags = types.MethodType(immediate_tags, thread_safe_result)
    return thread_safe_result

def runsuite(suite, parallel):
    suite_to_run = suite
    if parallel > 1:
        from concurrencytest import ConcurrentTestSuite, fork_for_tests
        if not WiredTigerTestCase._globalSetup:
            WiredTigerTestCase.globalSetup()
        WiredTigerTestCase._concurrent = True
        suite_to_run = ConcurrentTestSuite(suite, fork_for_tests(parallel), wrap_result=wrap_result_for_tags)
    try:
        if WiredTigerTestCase._randomseed:
            WiredTigerTestCase.prout("Starting test suite with seedw={0} and seedz={1}. Rerun this test with -seed {0}.{1} to get the same randomness"
                .format(str(WiredTigerTestCase._seeds[0]), str(WiredTigerTestCase._seeds[1])))
        result_class = None
        if WiredTigerTestCase._verbose > 1:
            result_class = test_result.PidAwareTextTestResult
        result = unittest.TextTestRunner(
            verbosity=WiredTigerTestCase._verbose, resultclass=result_class).run(suite_to_run)
        WiredTigerTestCase.finalReport()
        return result
    except BaseException as e:
        # This should not happen for regular test errors, unittest should catch everything
        print("[pid:{}]: ERROR: running test: {}".format(os.getpid(), e))
        raise e

def run(name='__main__'):
    result = runsuite(unittest.TestLoader().loadTestsFromName(name), False)
    sys.exit(0 if result.wasSuccessful() else 1)
