#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base04.py
#	Test that tables are reconciled correctly when they are empty.
#

import unittest
import wiredtiger
from wiredtiger import WT_NOTFOUND
import wttest

class test_base04(wttest.WiredTigerTestCase):
	'''Test various tree types becoming empty'''

	tablename = 'table:test_base04'

	def __init__(self, *args, **kwargs):
		wttest.WiredTigerTestCase.__init__(self, *args, **kwargs)
		self.reconcile = False

	def reopen(self):
		self.conn.close()
		self.conn = wiredtiger.wiredtiger_open('.', None)
		self.session = self.conn.open_session()

	def create_table(self):
		self.pr('create table')
		self.session.create(self.tablename, 'key_format=S,value_format=S')

	def drop_table(self):
		self.pr('drop table')
		self.session.drop(self.tablename, None)

	def cursor(self):
		self.pr('open cursor')
		return self.session.open_cursor(self.tablename, None, None)

	def check_exists(self, key, expected):
		cursor = self.cursor()
		cursor.set_key(key)
		self.pr('search')
		self.assertEqual(cursor.search(), expected)
		self.pr('closing cursor')
		cursor.close(None)

	def insert(self, key, value):
		self.pr('insert')
		cursor = self.cursor()
		cursor.set_key(key);
		cursor.set_value(value)
		cursor.insert()
		cursor.close(None)
		if self.reconcile:
			self.reopen()

	def remove(self, key):
		self.pr('remove')
		cursor = self.cursor()
		cursor.set_key(key);
		cursor.remove()
		cursor.close(None)
		if self.reconcile:
			self.reopen()

	def test_empty(self):
		'''Create a table, look for a nonexistent key'''
		self.create_table()
		self.check_exists('somekey', WT_NOTFOUND)
		self.drop_table()

	def test_insert(self):
		'''Create a table, add a key, get it back'''
		for self.reconcile in (False, True):
			self.create_table()
			self.insert('key1', 'value1')
			self.check_exists('key1', 0)
			self.drop_table()

	def test_insert_delete(self):
		'''Create a table, add a key, get it back'''
		for reconcile in (False, True):
			self.create_table()
			self.insert('key1', 'value1')
			self.check_exists('key1', 0)
			self.remove('key1')
			self.check_exists('key1', WT_NOTFOUND)
			self.drop_table()

if __name__ == '__main__':
	wttest.run()
