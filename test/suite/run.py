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

import os
import sys
import unittest
import glob

# Set paths
suitedir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(suitedir))
wt_builddir = os.path.join(wt_disttop, 'build_posix')

# Cannot import wiredtiger and supporting utils until we set up paths
sys.path.append(wt_builddir)
sys.path.append(os.path.join(wt_disttop, 'lang', 'python'))
sys.path.append(os.path.join(wt_disttop, 'test', 'suiteutil'))

import wttest
from testscenarios.scenarios import generate_scenarios

# These may be needed on some systems
#os.environ['LD_LIBRARY_PATH'] = os.environ['DYLD_LIBRARY_PATH'] = os.path.join(wt_builddir, '.libs')

#export ARCH="x86_64"  # may be needed on OS/X

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


tests = unittest.TestSuite()

# Without arguments, do discovery
if len(sys.argv) < 2:
	# Use the backport of Python 2.7+ unittest discover module.
	# (Under a BSD license, so we include a copy in our tree for simplicity.)
	from discover import defaultTestLoader as loader
	tests.addTests(generate_scenarios(loader.discover(suitedir)))

# Otherwise, turn numbers and ranges into test module names
preserve = False
for arg in sys.argv[1:]:
	from unittest import defaultTestLoader as loader

        # Command line options
        if arg[0] == '-':
                option = arg[1:]
                if option == 'preserve' or option == 'p':
                        preserve = True
                        continue

        testsFromArg(tests, loader, arg)

wttest.WiredTigerTestCase.globallyPreserveFiles(preserve)
result = wttest.runsuite(tests)
sys.exit(not result.wasSuccessful())
