#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# WiredTigerTestCase
#	parent class for all test cases
#

import os
import sys
import unittest
import glob

# Set paths
suitedir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(suitedir))
wt_builddir = os.path.join(wt_disttop, 'build_posix')

# Cannot import wiredtiger and supporting utils until we set up paths
sys.path.append(os.path.join(wt_builddir, 'lang', 'python'))
sys.path.append(os.path.join(wt_disttop, 'lang', 'python'))
sys.path.append(os.path.join(wt_disttop, 'test', 'suiteutil'))

import wttest
from testscenarios.scenarios import generate_scenarios

def usage():
    print 'Usage:\n\
  $ cd build_posix\n\
  $ python ../test/suite/run.py [ options ] [ tests ]\n\
\n\
Options:\n\
  -d   | --debug          run with \'pdb\', the python debugger\n\
  -g   | --gdb            all subprocesses (like calls to wt) invoke gdb\n\
  -h   | --help           show this message\n\
  -p   | --preserve       preserve output files in WT_TEST/<testname>\n\
  -t   | --timestamp      name WT_TEST according to timestamp\n\
  -v N | --verbose N      set verboseness to N (0<=N<=3, default=1)\n\
\n\
Tests:\n\
  may be a file name in test/suite: (e.g. test_base01.py)\n\
  may be a subsuite name (e.g. \'base\' runs test_base*.py)\n\
'

def addScenarioTests(tests, loader, testname):
    loaded = loader.loadTestsFromName(testname)
    tests.addTests(generate_scenarios(loaded))

def testsFromArg(tests, loader, arg):
    # If a group of test is mentioned, do all tests in that group
    # e.g. 'run.py base'
    groupedfiles = glob.glob(suitedir + os.sep + 'test_' + arg + '*.py')
    if len(groupedfiles) > 0:
        for file in groupedfiles:
            testsFromArg(tests, loader, os.path.basename(file))
        return

    # Explicit test class names
    if not arg[0].isdigit():
        if arg.endswith('.py'):
            arg = arg[:-3]
        addScenarioTests(tests, loader, arg)
        return

    # Deal with ranges
    if '-' in arg:
        start, end = (int(a) for a in arg.split('-'))
    else:
        start, end = int(arg), int(arg)
    for t in xrange(start, end+1):
        addScenarioTests(tests, loader, 'test%03d' % t)

if __name__ == '__main__':
    tests = unittest.TestSuite()

    # Turn numbers and ranges into test module names
    preserve = timestamp = debug = gdbSub = False
    verbose = 1
    args = sys.argv[1:]
    testargs = []
    while len(args) > 0:
        arg = args.pop(0)
        from unittest import defaultTestLoader as loader

        # Command line options
        if arg[0] == '-':
            option = arg[1:]
            if option == '-debug' or option == 'd':
                debug = True
                continue
            if option == '-preserve' or option == 'p':
                preserve = True
                continue
            if option == '-timestamp' or option == 't':
                timestamp = True
                continue
            if option == '-gdb' or option == 'g':
                gdbSub = True
                continue
            if option == '-help' or option == 'h':
                usage()
                sys.exit(True)
            if option == '-verbose' or option == 'v':
                if len(args) == 0:
                    usage()
                    sys.exit(False)
                verbose = int(args.pop(0))
                continue
            print 'unknown arg: ' + arg
            usage()
            sys.exit(False)
        testargs.append(arg)

    # Without arguments, do discovery
    if len(testargs) == 0:
        from discover import defaultTestLoader as loader
        suites = loader.discover(suitedir)
        suites = sorted(suites, key=lambda c: str(list(c)[0]))
        tests.addTests(generate_scenarios(suites))
    else:
        for arg in testargs:
            testsFromArg(tests, loader, arg)
        
    wttest.WiredTigerTestCase.globalSetup(preserve, timestamp, gdbSub, verbose)

    if debug:
        import pdb
        pdb.set_trace()

    result = wttest.runsuite(tests)
    sys.exit(not result.wasSuccessful())
