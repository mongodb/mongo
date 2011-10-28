#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util04.py
# 	Utilities: wt drop
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os

class test_util04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util04.a'
    nentries = 1000

    def test_drop_process(self):
        """
        Test drop in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)

        self.assertTrue(os.path.exists(self.tablename + ".wt"))
        self.runWt(["drop", "table:" + self.tablename])

        self.assertFalse(os.path.exists(self.tablename + ".wt"))
        self.assertRaises(WiredTigerError, lambda: self.session.open_cursor('table:' + self.tablename, None, None))


if __name__ == '__main__':
    wttest.run()
