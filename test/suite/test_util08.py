#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util08.py
# 	Utilities: wt copyright
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess

class test_util08(wttest.WiredTigerTestCase, suite_subprocess):
    def test_copyright(self):
        """
        Test copyright in a 'wt' process
        """
        outfile = "copyrightout.txt"
        self.runWt(["copyright"], outfilename=outfile)
        with open(outfile, 'r') as f:
            text = f.read(1000)
            self.assertTrue('Copyright' in text)


if __name__ == '__main__':
    wttest.run()
