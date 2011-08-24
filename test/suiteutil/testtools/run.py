# Copyright (c) 2009 testtools developers. See LICENSE for details.

"""python -m testtools.run testspec [testspec...]

Run some tests with the testtools extended API.

For instance, to run the testtools test suite.
 $ python -m testtools.run testtools.tests.test_suite
"""

import os
import unittest
import sys

from testtools import TextTestResult
from testtools.compat import classtypes, istext, unicode_output_stream
from testtools.testsuite import iterate_tests


defaultTestLoader = unittest.defaultTestLoader
defaultTestLoaderCls = unittest.TestLoader

if getattr(defaultTestLoader, 'discover', None) is None:
    try:
        import discover
        defaultTestLoader = discover.DiscoveringTestLoader()
        defaultTestLoaderCls = discover.DiscoveringTestLoader
        have_discover = True
    except ImportError:
        have_discover = False
else:
    have_discover = True


class TestToolsTestRunner(object):
    """ A thunk object to support unittest.TestProgram."""

    def __init__(self, stdout):
        self.stdout = stdout

    def run(self, test):
        "Run the given test case or test suite."
        result = TextTestResult(unicode_output_stream(self.stdout))
        result.startTestRun()
        try:
            return test.run(result)
        finally:
            result.stopTestRun()


####################
# Taken from python 2.7 and slightly modified for compatibility with
# older versions. Delete when 2.7 is the oldest supported version.
# Modifications:
#  - Use have_discover to raise an error if the user tries to use
#    discovery on an old version and doesn't have discover installed.
#  - If --catch is given check that installHandler is available, as
#    it won't be on old python versions.
#  - print calls have been been made single-source python3 compatibile.
#  - exception handling likewise.
#  - The default help has been changed to USAGE_AS_MAIN and USAGE_FROM_MODULE
#    removed.
#  - A tweak has been added to detect 'python -m *.run' and use a
#    better progName in that case.
#  - self.module is more comprehensively set to None when being invoked from
#    the commandline - __name__ is used as a sentinel value.
#  - --list has been added which can list tests (should be upstreamed).
#  - --load-list has been added which can reduce the tests used (should be
#    upstreamed).
#  - The limitation of using getopt is declared to the user.

FAILFAST     = "  -f, --failfast   Stop on first failure\n"
CATCHBREAK   = "  -c, --catch      Catch control-C and display results\n"
BUFFEROUTPUT = "  -b, --buffer     Buffer stdout and stderr during test runs\n"

USAGE_AS_MAIN = """\
Usage: %(progName)s [options] [tests]

Options:
  -h, --help       Show this message
  -v, --verbose    Verbose output
  -q, --quiet      Minimal output
  -l, --list       List tests rather than executing them.
  --load-list      Specifies a file containing test ids, only tests matching
                   those ids are executed.
%(failfast)s%(catchbreak)s%(buffer)s
Examples:
  %(progName)s test_module               - run tests from test_module
  %(progName)s module.TestClass          - run tests from module.TestClass
  %(progName)s module.Class.test_method  - run specified test method

All options must come before [tests].  [tests] can be a list of any number of
test modules, classes and test methods.

Alternative Usage: %(progName)s discover [options]

Options:
  -v, --verbose    Verbose output
%(failfast)s%(catchbreak)s%(buffer)s  -s directory     Directory to start discovery ('.' default)
  -p pattern       Pattern to match test files ('test*.py' default)
  -t directory     Top level directory of project (default to
                   start directory)
  -l, --list       List tests rather than executing them.
  --load-list      Specifies a file containing test ids, only tests matching
                   those ids are executed.

For test discovery all test modules must be importable from the top
level directory of the project.
"""


class TestProgram(object):
    """A command-line program that runs a set of tests; this is primarily
       for making test modules conveniently executable.
    """
    USAGE = USAGE_AS_MAIN

    # defaults for testing
    failfast = catchbreak = buffer = progName = None

    def __init__(self, module=__name__, defaultTest=None, argv=None,
                    testRunner=None, testLoader=defaultTestLoader,
                    exit=True, verbosity=1, failfast=None, catchbreak=None,
                    buffer=None, stdout=None):
        if module == __name__:
            self.module = None
        elif istext(module):
            self.module = __import__(module)
            for part in module.split('.')[1:]:
                self.module = getattr(self.module, part)
        else:
            self.module = module
        if argv is None:
            argv = sys.argv
        if stdout is None:
            stdout = sys.stdout

        self.exit = exit
        self.failfast = failfast
        self.catchbreak = catchbreak
        self.verbosity = verbosity
        self.buffer = buffer
        self.defaultTest = defaultTest
        self.listtests = False
        self.load_list = None
        self.testRunner = testRunner
        self.testLoader = testLoader
        progName = argv[0]
        if progName.endswith('%srun.py' % os.path.sep):
            elements = progName.split(os.path.sep)
            progName = '%s.run' % elements[-2]
        else:
            progName = os.path.basename(argv[0])
        self.progName = progName
        self.parseArgs(argv)
        if self.load_list:
            # TODO: preserve existing suites (like testresources does in
            # OptimisingTestSuite.add, but with a standard protocol).
            # This is needed because the load_tests hook allows arbitrary
            # suites, even if that is rarely used.
            source = file(self.load_list, 'rb')
            try:
                lines = source.readlines()
            finally:
                source.close()
            test_ids = set(line.strip() for line in lines)
            filtered = unittest.TestSuite()
            for test in iterate_tests(self.test):
                if test.id() in test_ids:
                    filtered.addTest(test)
            self.test = filtered
        if not self.listtests:
            self.runTests()
        else:
            for test in iterate_tests(self.test):
                stdout.write('%s\n' % test.id())

    def usageExit(self, msg=None):
        if msg:
            print(msg)
        usage = {'progName': self.progName, 'catchbreak': '', 'failfast': '',
                 'buffer': ''}
        if self.failfast != False:
            usage['failfast'] = FAILFAST
        if self.catchbreak != False:
            usage['catchbreak'] = CATCHBREAK
        if self.buffer != False:
            usage['buffer'] = BUFFEROUTPUT
        print(self.USAGE % usage)
        sys.exit(2)

    def parseArgs(self, argv):
        if len(argv) > 1 and argv[1].lower() == 'discover':
            self._do_discovery(argv[2:])
            return

        import getopt
        long_opts = ['help', 'verbose', 'quiet', 'failfast', 'catch', 'buffer',
            'list', 'load-list=']
        try:
            options, args = getopt.getopt(argv[1:], 'hHvqfcbl', long_opts)
            for opt, value in options:
                if opt in ('-h','-H','--help'):
                    self.usageExit()
                if opt in ('-q','--quiet'):
                    self.verbosity = 0
                if opt in ('-v','--verbose'):
                    self.verbosity = 2
                if opt in ('-f','--failfast'):
                    if self.failfast is None:
                        self.failfast = True
                    # Should this raise an exception if -f is not valid?
                if opt in ('-c','--catch'):
                    if self.catchbreak is None:
                        self.catchbreak = True
                    # Should this raise an exception if -c is not valid?
                if opt in ('-b','--buffer'):
                    if self.buffer is None:
                        self.buffer = True
                    # Should this raise an exception if -b is not valid?
                if opt in ('-l', '--list'):
                    self.listtests = True
                if opt == '--load-list':
                    self.load_list = value
            if len(args) == 0 and self.defaultTest is None:
                # createTests will load tests from self.module
                self.testNames = None
            elif len(args) > 0:
                self.testNames = args
            else:
                self.testNames = (self.defaultTest,)
            self.createTests()
        except getopt.error:
            self.usageExit(sys.exc_info()[1])

    def createTests(self):
        if self.testNames is None:
            self.test = self.testLoader.loadTestsFromModule(self.module)
        else:
            self.test = self.testLoader.loadTestsFromNames(self.testNames,
                                                           self.module)

    def _do_discovery(self, argv, Loader=defaultTestLoaderCls):
        # handle command line args for test discovery
        if not have_discover:
            raise AssertionError("Unable to use discovery, must use python 2.7 "
                    "or greater, or install the discover package.")
        self.progName = '%s discover' % self.progName
        import optparse
        parser = optparse.OptionParser()
        parser.prog = self.progName
        parser.add_option('-v', '--verbose', dest='verbose', default=False,
                          help='Verbose output', action='store_true')
        if self.failfast != False:
            parser.add_option('-f', '--failfast', dest='failfast', default=False,
                              help='Stop on first fail or error',
                              action='store_true')
        if self.catchbreak != False:
            parser.add_option('-c', '--catch', dest='catchbreak', default=False,
                              help='Catch ctrl-C and display results so far',
                              action='store_true')
        if self.buffer != False:
            parser.add_option('-b', '--buffer', dest='buffer', default=False,
                              help='Buffer stdout and stderr during tests',
                              action='store_true')
        parser.add_option('-s', '--start-directory', dest='start', default='.',
                          help="Directory to start discovery ('.' default)")
        parser.add_option('-p', '--pattern', dest='pattern', default='test*.py',
                          help="Pattern to match tests ('test*.py' default)")
        parser.add_option('-t', '--top-level-directory', dest='top', default=None,
                          help='Top level directory of project (defaults to start directory)')
        parser.add_option('-l', '--list', dest='listtests', default=False,
                          help='List tests rather than running them.')
        parser.add_option('--load-list', dest='load_list', default=None,
                          help='Specify a filename containing the test ids to use.')

        options, args = parser.parse_args(argv)
        if len(args) > 3:
            self.usageExit()

        for name, value in zip(('start', 'pattern', 'top'), args):
            setattr(options, name, value)

        # only set options from the parsing here
        # if they weren't set explicitly in the constructor
        if self.failfast is None:
            self.failfast = options.failfast
        if self.catchbreak is None:
            self.catchbreak = options.catchbreak
        if self.buffer is None:
            self.buffer = options.buffer
        self.listtests = options.listtests
        self.load_list = options.load_list

        if options.verbose:
            self.verbosity = 2

        start_dir = options.start
        pattern = options.pattern
        top_level_dir = options.top

        loader = Loader()
        self.test = loader.discover(start_dir, pattern, top_level_dir)

    def runTests(self):
        if (self.catchbreak
            and getattr(unittest, 'installHandler', None) is not None):
            unittest.installHandler()
        if self.testRunner is None:
            self.testRunner = runner.TextTestRunner
        if isinstance(self.testRunner, classtypes()):
            try:
                testRunner = self.testRunner(verbosity=self.verbosity,
                                             failfast=self.failfast,
                                             buffer=self.buffer)
            except TypeError:
                # didn't accept the verbosity, buffer or failfast arguments
                testRunner = self.testRunner()
        else:
            # it is assumed to be a TestRunner instance
            testRunner = self.testRunner
        self.result = testRunner.run(self.test)
        if self.exit:
            sys.exit(not self.result.wasSuccessful())
################

def main(argv, stdout):
    runner = TestToolsTestRunner(stdout)
    program = TestProgram(argv=argv, testRunner=runner, stdout=stdout)

if __name__ == '__main__':
    main(sys.argv, sys.stdout)
