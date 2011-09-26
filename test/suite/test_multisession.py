#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base03.py
# 	Cursor operations
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class test_multisession(wttest.WiredTigerTestCase):
    def test_one_session(self):
        self.assertRaises(WiredTigerError, self.conn.open_session)

    def test_two_sessions(self):
		self.conn.close()
		self.conn = wiredtiger.wiredtiger_open('.', 'multithread')
		self.session = self.conn.open_session()
		session2 = self.conn.open_session()

if __name__ == '__main__':
    wttest.run()
