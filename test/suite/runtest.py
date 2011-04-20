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
wt_builddir = wt_disttop + os.sep + 'build_posix'

# Cannot import wiredtiger until we set up paths
sys.path.append(wt_builddir)

# May be needed on some systems?
#os.environ['LD_LIBRARY_PATH'] = builddir
#os.environ['DYLD_LIBRARY_PATH'] = builddir   # for OS/X

import wiredtiger
import wttest

#export ARCH="x86_64"  # may be needed on OS/X

def canonicalizeName(n):
    """
    convert numeric value to string, e.g. 1->"001"
    """
    _s = "" + n;
    while len(_s) < 3:
        _s = "0" + _s
    return _s

tests = []
for arg in sys.argv[1:]:
    # TODO: handle ranges, commas and combinations 1-4,6,12-14
    tests.append(canonicalizeName(arg))

suite = unittest.TestSuite()
for test in tests:
    testname = 'test' + test
    module = __import__(testname)
    cl = getattr(module, testname)
    suite.addTest(unittest.TestLoader().loadTestsFromTestCase(cl))

wttest.runsuite(suite)
