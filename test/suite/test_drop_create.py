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

class test_drop_create(wttest.WiredTigerTestCase):
    def test_drop_create(self):
		s, self.session = self.session, None
		self.assertEqual(s.close(), 0)

		for config in [None, 'key_format=S,value_format=S', None]:
			s = self.conn.open_session()
			self.assertEqual(s.drop("table:test", "force"), 0)
			self.assertEqual(s.create("table:test", config), 0)
			self.assertEqual(s.drop("table:test"), 0)
			self.assertEqual(s.close(), 0)
			s = self.conn.open_session()
			self.assertNotEqual(s, None)
			self.assertEqual(s.create("table:test", config), 0)
			self.assertEqual(s.close(), 0)

if __name__ == '__main__':
    wttest.run()
