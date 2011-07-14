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

# Set paths
suitedir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(suitedir))
wt_builddir = os.path.join(wt_disttop, 'build_posix')

# Cannot import wiredtiger until we set up paths
sys.path.append(wt_builddir)
sys.path.append(os.path.join(wt_disttop, 'lang', 'python'))

# These may be needed on some systems
#os.environ['LD_LIBRARY_PATH'] = os.environ['DYLD_LIBRARY_PATH'] = wt_builddir

#export ARCH="x86_64"  # may be needed on OS/X


tests = unittest.TestSuite()

# Without arguments, do discovery
if len(sys.argv) < 2:
	# Use the backport of Python 2.7+ unittest discover module.
	# (Under a BSD license, so we include a copy in our tree for simplicity.)
	from discover import defaultTestLoader as loader
	tests.addTests(loader.discover(suitedir))

# Otherwise, turn numbers and ranges into test module names
for arg in sys.argv[1:]:
	from unittest import defaultTestLoader as loader
	# Deal with ranges
	if '-' in arg:
		start, end = (int(a) for a in arg.split('-'))
	else:
		start, end = int(arg), int(arg)
	for t in xrange(start, end+1):
		tests.addTests(loader.loadTestsFromName('test%03d' % t))

import wttest
result = wttest.runsuite(tests)
sys.exit(not result.wasSuccessful())
