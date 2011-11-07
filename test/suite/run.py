#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
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
  -p | -preserve       preserve output files in WT_TEST/<testname>\n\
  -t | -timestamp      name WT_TEST according to timestamp\n\
  -d | -debug          run with \'pdb\', the python debugger\n\
  -g | -gdb            all subprocesses (like calls to wt) invoke gdb\n\
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

    # Without arguments, do discovery
    if len(sys.argv) < 2:
        from discover import defaultTestLoader as loader
        tests.addTests(generate_scenarios(loader.discover(suitedir)))

    # Otherwise, turn numbers and ranges into test module names
    preserve = timestamp = debug = gdbSub = False
    for arg in sys.argv[1:]:
        from unittest import defaultTestLoader as loader

        # Command line options
        if arg[0] == '-':
            option = arg[1:]
            if option == 'preserve' or option == 'p':
                preserve = True
                continue
            if option == 'timestamp' or option == 't':
                timestamp = True
                continue
            if option == 'debug' or option == 'd':
                import pdb
                debug = True
                continue
            if option == 'gdb' or option == 'g':
                gdbSub = True
                continue
            usage()
            sys.exit(False)

        testsFromArg(tests, loader, arg)

        if debug:
                pdb.set_trace()
    wttest.WiredTigerTestCase.globalSetup(preserve, timestamp, gdbSub)
    result = wttest.runsuite(tests)
    sys.exit(not result.wasSuccessful())
