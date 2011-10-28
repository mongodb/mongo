#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util05.py
# 	Utilities: wt verify
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os

class test_util05(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util05.a'
    nentries = 1000

    def test_verify_process(self):
        """
        Test verify in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)

        # Insert some simple entries 
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        key = ''
        for i in range(0, self.nentries):
            key += str(i)
            val = key + key
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()
        cursor.close()

        self.runWt(["verify", "table:" + self.tablename])

    def test_verify_process_empty(self):
        """
        Test verify in a 'wt' process, using an empty table
        """
        self.skipTest('**** TODO: this test is broken, we expect verify to work on an empty table ****')

        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)

        # Run verify with an empty table
        self.runWt(["verify", "table:" + self.tablename])


if __name__ == '__main__':
    wttest.run()
