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

import inspect, os, re, shutil, sys, time, traceback, unittest

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
        self.ignore_regex = None

    def setIgnorePattern(self, regex):
        self.ignore_regex = regex

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
        if AbstractWiredTigerTestCase._ignoreStdout:
            return
        if self.file != None:
            self.file.flush()
        new_size = os.path.getsize(self.filename)
        if self.ignore_regex is None:
            return self.expectpos < new_size

        gotstr = self.readFileFrom(self.filename, self.expectpos, new_size - self.expectpos)
        for line in list(filter(None, gotstr.split('\n'))):
            if self.ignore_regex.search(line) is None:
                return True
        return False

    def check(self, testcase):
        """
        Check to see that there is no unexpected output in the captured output
        file.  If there is, raise it as a test failure.
        This is generally called after 'release' is called.
        """
        if self.hasUnexpectedOutput(testcase):
            contents = self.readFileFrom(self.filename, self.expectpos, 10000)
            AbstractWiredTigerTestCase.prout('ERROR: ' + self.filename +
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

    def checkAdditionalPattern(self, testcase, pat, re_flags = 0, maxchars = 1500):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        if self.file != None:
            self.file.flush()
        gotstr = self.readFileFrom(self.filename, self.expectpos, maxchars)
        if re.search(pat, gotstr, re_flags) == None:
            testcase.fail('in ' + self.desc +
                          ', expected pattern "' + pat + '", but got "' +
                          gotstr + '"')
        self.expectpos = os.path.getsize(self.filename)

    def checkCustomValidator(self, testcase, f):
        """
        Check to see that an additional string has been added to the
        output file.  If it has not, raise it as a test failure.
        In any case, reset the expected pos to account for the new output.
        """
        if self.file != None:
            self.file.flush()

        # Custom validators probably don't want to see truncated output.
        # Give them the whole string.
        new_expectpos = os.path.getsize(self.filename)
        diff = new_expectpos - self.expectpos
        gotstr = self.readFileFrom(self.filename, self.expectpos, diff)
        try:
            f(gotstr)
        except Exception as e:
            testcase.fail('in ' + self.desc +
                          ', custom validator failed: ' + str(e))
        self.expectpos = new_expectpos

class AbstractWiredTigerTestCase(unittest.TestCase):
    '''
    The parent class of all WiredTiger Python-based test cases, regardless of their test suite,
    whether they are a part of the Python test suite or compatibility test suite.
    '''
    _ioSetup = False
    _printOnceSeen = {}
    _ttyDescriptor = None   # set this early, to allow tty() to be called any time.

    # Placeholder configuration, in the case no one calls the setup functions.
    _dupout = sys.stdout
    _ignoreStdout = False
    _parentTestdir = None
    _resultFile = sys.stdout
    _stderr = sys.stderr
    _stdout = sys.stdout
    _verbose = 1

    #
    # Test case - common
    #

    def __init__(self, *args, **kwargs):
        '''
        Initialize the test case.
        '''
        if hasattr(self, 'scenarios'):
            assert(self.scenarios is None or len(self.scenarios) == len(dict(self.scenarios)))
        super().__init__(*args, **kwargs)
        self.ignore_regex = None

    def failed(self):
        '''
        Determine whether the test failed.
        '''
        # This approach works for all our support Python versions and
        # is suggested by one of the answers in:
        # https://stackoverflow.com/questions/4414234/getting-pythons-unittest-results-in-a-teardown-method
        # In addition, check to make sure exc_info is "clean", because
        # the ConcurrencyTestSuite in Python2 indicates failures using that.
        if hasattr(self, '_outcome'):  # Python 3.4+
            if hasattr(self._outcome, 'errors'):  # Python 3.4 - 3.10
                result = self.defaultTestResult()  # these 2 methods have no side effects
                self._feedErrorsToResult(result, self._outcome.errors)
            else:  # Python 3.11+
                result = self._outcome.result
        else:  # Python 3.2 - 3.3 or 3.0 - 3.1 and 2.7
            result = getattr(self, '_outcomeForDoCleanups', self._resultForDoCleanups)
        error = self.list2reason(result, 'errors')
        failure = self.list2reason(result, 'failures')
        exc_failure = (sys.exc_info() != (None, None, None))

        self._failed = error or failure or exc_failure
        return self._failed

    def system(self, command):
        '''
        Run a command, fail the test if the command execution failed.
        '''
        self.assertEqual(os.system(command), 0)

    #
    # Test directory setup
    #

    @staticmethod
    def setupTestDir(dir = 'WT_TEST', preserveFiles = False, removeAtStart = True,
                     useTimestamp = False):
        '''
        Set up the test directory.
        '''
        d = dir
        if useTimestamp:
            d += '.' + time.strftime('%Y%m%d-%H%M%S', time.localtime())
        if removeAtStart:
            shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)
        AbstractWiredTigerTestCase._origcwd = os.getcwd()
        AbstractWiredTigerTestCase._parentTestdir = os.path.abspath(d)
        AbstractWiredTigerTestCase._preserveFiles = preserveFiles

    #
    # Standard I/O
    #

    @staticmethod
    def setupIO(resultFileName = 'results.txt', ignoreStdout = False, verbose = 1):
        '''
        Set up I/O for the test.
        '''
        if AbstractWiredTigerTestCase._parentTestdir is None:
            raise Exception('setupTestDir() must be called before setupIO()')
        resultFilePath = os.path.abspath(
            os.path.join(AbstractWiredTigerTestCase._parentTestdir, resultFileName))

        AbstractWiredTigerTestCase._ignoreStdout = ignoreStdout
        AbstractWiredTigerTestCase._resultFileName = resultFilePath
        AbstractWiredTigerTestCase._verbose = verbose

        AbstractWiredTigerTestCase._stdout = sys.stdout
        AbstractWiredTigerTestCase._stderr = sys.stderr

        AbstractWiredTigerTestCase.finishSetupIO()
        AbstractWiredTigerTestCase._ioSetup = True

    @staticmethod
    def finishSetupIO():
        '''
        Finish setting up I/O for the test.
        '''
        resultFileName = AbstractWiredTigerTestCase._resultFileName

        AbstractWiredTigerTestCase._dupout = os.dup(sys.stdout.fileno())
        AbstractWiredTigerTestCase._resultFile = open(resultFileName, "a", 1)  # line buffered

        # We don't have a lot of output, but we want to see it right away.
        sys.stdout.reconfigure(line_buffering=True)

    def fdSetUp(self):
        '''
        Set up stderr/stdout after a test.
        '''
        self.captureout = CapturedFd('stdout.txt', 'standard output')
        self.captureerr = CapturedFd('stderr.txt', 'error output')
        sys.stdout = self.captureout.capture()
        sys.stderr = self.captureerr.capture()
        if self.ignore_regex is not None:
            self.captureout.setIgnorePattern(self.ignore_regex)

    def fdTearDown(self):
        '''
        Restore stderr/stdout after a test.
        '''
        self.captureout.release()
        self.captureerr.release()
        sys.stdout = AbstractWiredTigerTestCase._stdout
        sys.stderr = AbstractWiredTigerTestCase._stderr

    @staticmethod
    def printOnce(msg):
        # There's a race condition with multiple threads,
        # but we won't worry about it.  We err on the side
        # of printing the message too many times.
        if not msg in AbstractWiredTigerTestCase._printOnceSeen:
            AbstractWiredTigerTestCase._printOnceSeen[msg] = msg
            AbstractWiredTigerTestCase.prout(msg)

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
        if level <= AbstractWiredTigerTestCase._verbose:
            AbstractWiredTigerTestCase.prout(message)

    def verbose(self, level, message):
        AbstractWiredTigerTestCase.printVerbose(level, message)

    def prout(self, s):
        AbstractWiredTigerTestCase.prout(s)

    @staticmethod
    def prout(s):
        os.write(AbstractWiredTigerTestCase._dupout,
                 str.encode("[pid:{}]: {}\n".format(os.getpid(), s)))

    def pr(self, s):
        '''
        Print a progress line for testing
        '''
        msg = '    ' + self.shortid() + ': ' + s
        AbstractWiredTigerTestCase._resultFile.write(msg + '\n')

    def prhead(self, s, *beginning):
        '''
        print a header line for testing, something important
        '''
        msg = ''
        if len(beginning) > 0:
            msg += '\n'
        msg += '  ' + self.shortid() + ': ' + s
        self.prout(msg)
        AbstractWiredTigerTestCase._resultFile.write(msg + '\n')

    def prexception(self, excinfo):
        AbstractWiredTigerTestCase._resultFile.write('\n')
        traceback.print_exception(excinfo[0], excinfo[1], excinfo[2], None,
                                  AbstractWiredTigerTestCase._resultFile)
        AbstractWiredTigerTestCase._resultFile.write('\n')

    #
    # TTY I/O (useful for debugging)
    #

    def tty(self, message):
        '''
        Print directly to tty, useful for debugging.
        '''
        AbstractWiredTigerTestCase.tty(message)

    @staticmethod
    def tty(message):
        '''
        Print directly to tty, useful for debugging.
        '''
        if AbstractWiredTigerTestCase._ttyDescriptor == None:
            AbstractWiredTigerTestCase._ttyDescriptor = open('/dev/tty', 'w')
        full_message = "[pid:{}]: {}\n".format(os.getpid(), message)
        AbstractWiredTigerTestCase._ttyDescriptor.write(full_message)

    def ttyVerbose(self, level, message):
        AbstractWiredTigerTestCase.ttyVerbose(level, message)

    @staticmethod
    def ttyVerbose(level, message):
        if level <= AbstractWiredTigerTestCase._verbose:
            AbstractWiredTigerTestCase.tty(message)

    #
    # Random number generation support
    #

    @staticmethod
    def setupRandom(seedw = 0, seedz = 0):
        '''
        Set up the random number generation.
        '''
        AbstractWiredTigerTestCase._seeds = [521288629, 362436069]
        AbstractWiredTigerTestCase._randomseed = False
        if seedw != 0 and seedz != 0:
            AbstractWiredTigerTestCase._randomseed = True
            AbstractWiredTigerTestCase._seeds = [seedw, seedz]

    #
    # Test case introspection
    #

    def __str__(self):
        # when running with scenarios, if the number_scenarios() method
        # is used, then each scenario is given a number, which can
        # help distinguish tests.
        scen = ''
        if hasattr(self, 'scenario_number') and hasattr(self, 'scenario_name'):
            scen = ' -s ' + str(self.scenario_number) + \
                   ' (' + self.scenario_name + ')'
        return self.simpleName() + scen

    def class_name(self):
        return self.__class__.__name__

    def module_file(self):
        '''
        Get the (base) file name for this test.
        '''
        return os.path.basename(inspect.getfile(self.__class__))

    def module_name(self):
        '''
        Get the module name for this test.
        '''
        return self.module_file().replace('.py', '')

    def current_test_id(self):
        '''
        Return a test ID. Use this instead of the actual id() function, because we lose its context
        during compatibility tests.
        '''
        if hasattr(self, '_id'):
            return getattr(self, '_id')
        self._id = self.id()
        return self._id

    def shortid(self):
        return self.current_test_id().replace("__main__.","")

    def sanitized_shortid(self):
        '''
        Return a name that is suitable for creating file system names.
        In particular, names with scenarios look like
        'test_file.test_file.test_funcname(scen1.scen2.scen3)'.
        So transform '(', but remove final ')'.
        '''
        name = self.shortid().translate(str.maketrans('($[]/ ','______', ')'))

        # Remove '<' and '>', because some qualified names contain strings such as "<locals>".
        name = name.replace('<', '_').replace('>', '_')

        # On OS/X, we can get name conflicts if names differ by case. Upper
        # case letters are uncommon in our python class and method names, so
        # we lowercase them and prefix with '@', e.g. "AbC" -> "@ab@c".
        return re.sub(r'[A-Z]', lambda x: '@' + x.group(0).lower(), name)

    def shortDesc(self):
        ret_str = ''
        if hasattr(self, 'scenario_number'):
            ret_str = ' -s ' + str(self.scenario_number)
        return self.simpleName() + ret_str

    def simpleName(self):
        # Prefer the saved method name, it's always correct if set.
        if hasattr(self, '_savedTestMethodName'):
            methodName = self._savedTestMethodName
        else:
            methodName = self._testMethodName
        return "%s.%s.%s" %  (self.__module__, self.class_name(), methodName)

    #
    # Debugging
    #

    def breakpoint(self):
        '''
        Set a Python breakpoint.  When this function is called,
        the python debugger will be called as described here:
          https://docs.python.org/3/library/pdb.html

        This can be used instead of the Python built-in "breakpoint",
        so that the terminal has proper I/O and a prompt appears, etc.

        Since the actual breakpoint is in this method, the developer will
        probably need to single step to get back to their calling function.
        '''
        import pdb, sys
        # Restore I/O to the controlling tty so we can
        # run the debugger.
        if os.name == "nt":
            # No solution has been tested here.
            pass
        else:
            sys.stdin = open('/dev/tty', 'r')
            sys.stdout = open('/dev/tty', 'w')
            sys.stderr = open('/dev/tty', 'w')
        self.printOnce("""
        ********
        You are now in the python debugger, type "help" for more information.
        Typing "s" will single step, returning you to the calling function. Common commands:
          list            -   show python source code
          s               -   single step
          n               -   next step
          b file:number   -   set a breakpoint
          p variable      -   print the value of a variable
          c               -   continue
        ********""")
        pdb.set_trace()

def getseed():
    '''
    Get the globally configured random seeds.
    '''
    return AbstractWiredTigerTestCase._seeds
