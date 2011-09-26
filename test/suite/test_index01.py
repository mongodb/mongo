#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test001.py
#	Basic operations
#

import unittest
import wiredtiger
from wiredtiger import WT_NOTFOUND
import wttest

class test_index01(wttest.WiredTigerTestCase):
	'''Test various tree types becoming empty'''

	basename = 'test_index01'
	tablename = 'table:' + basename
	indexbase = 'index:' + basename
	NUM_INDICES = 6
	index = ['%s:index%d' % (indexbase, i) for i in xrange(NUM_INDICES)]

	def reopen(self):
		self.conn.close()
		self.conn = wiredtiger.wiredtiger_open('.', None)
		self.session = self.conn.open_session()

	def create_table(self):
		self.pr('create table')
		self.session.create(self.tablename, 'key_format=Si,value_format=SSii,columns=(name,ID,dept,job,salary,year)')
		self.session.create(self.index[0], 'columns=(dept)')
		self.session.create(self.index[1], 'columns=(name,year)')
		self.session.create(self.index[2], 'columns=(salary)')
		self.session.create(self.index[3], 'columns=(dept,job,name)')
		self.session.create(self.index[4], 'columns=(name,ID)')
		self.session.create(self.index[5], 'columns=(ID,name)')

	def drop_table(self):
		self.pr('drop table')
		self.session.drop(self.tablename, None)

	def cursor(self, config=None):
		self.pr('open cursor')
		c = self.session.open_cursor(self.tablename, None, config)
		self.assertNotEqual(c, None)
		return c

	def index_cursor(self, i):
		self.pr('open index cursor(%d)' % i)
		c = self.session.open_cursor(self.index[i], None, None)
		self.assertNotEqual(c, None)
		return c

	def index_iter(self, i):
		cursor = self.index_cursor(i)
		for cols in cursor:
			yield cols
		cursor.close()

	def check_exists(self, name, ID, expected):
		cursor = self.cursor()
		cursor.set_key(name, ID)
		self.pr('search')
		self.assertEqual(cursor.search(), expected)
		self.pr('closing cursor')
		cursor.close(None)

	def insert(self, *cols):
		self.pr('insert')
		cursor = self.cursor()
		cursor.set_key(*cols[:2]);
		cursor.set_value(*cols[2:])
		self.assertEqual(cursor.insert(), 0)
		cursor.close(None)

	def insert_overwrite(self, *cols):
		self.pr('insert')
		cursor = self.cursor(config='overwrite')
		cursor.set_key(*cols[:2]);
		cursor.set_value(*cols[2:])
		self.assertEqual(cursor.insert(), 0)
		cursor.close(None)

	def update(self, *cols):
		self.pr('insert')
		cursor = self.cursor()
		cursor.set_key(*cols[:2]);
		cursor.set_value(*cols[2:])
		self.assertEqual(cursor.update(), 0)
		cursor.close(None)

	def remove(self, name, ID):
		self.pr('remove')
		cursor = self.cursor()
		cursor.set_key(name, ID);
		self.assertEqual(cursor.remove(), 0)
		cursor.close(None)

	def test_empty(self):
		'''Create a table, look for a nonexistent key'''
		self.create_table()
		self.check_exists('jones', 10, WT_NOTFOUND)
		for i in xrange(self.NUM_INDICES):
			self.assertEqual(list(self.index_iter(i)), [])
		self.drop_table()

	def test_insert(self):
		'''Create a table, add a key, get it back'''
		self.create_table()
		self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
		self.check_exists('smith', 1, 0)
		for i in xrange(self.NUM_INDICES):
			print 'Index(%d) contents:' % i
			print '\n'.join(repr(cols) for cols in self.index_iter(i))
			print
		self.drop_table()

	def test_update(self):
		'''Create a table, add a key, update it, get it back'''
		self.create_table()
		self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
		self.update('smith', 1, 'HR', 'janitor', 10000, 1970)
		self.check_exists('smith', 1, 0)
		for i in xrange(self.NUM_INDICES):
			print 'Index(%d) contents:' % i
			print '\n'.join(repr(cols) for cols in self.index_iter(i))
			print
		self.drop_table()

	def test_insert_overwrite(self):
		'''Create a table, add a key, insert-overwrite it,
		   insert-overwrite a nonexistent record, get them both back'''
		self.create_table()
		self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
		self.insert_overwrite('smith', 1, 'HR', 'janitor', 10000, 1970)
		self.insert_overwrite('jones', 2, 'IT', 'sysadmin', 50000, 1980)
		self.check_exists('smith', 1, 0)
		self.check_exists('jones', 2, 0)
		for i in xrange(self.NUM_INDICES):
			print 'Index(%d) contents:' % i
			print '\n'.join(repr(cols) for cols in self.index_iter(i))
			print
		self.drop_table()

	def test_insert_delete(self):
		'''Create a table, add a key, remove it'''
		self.create_table()
		self.insert('smith', 1, 'HR', 'manager', 100000, 1970)
		self.check_exists('smith', 1, 0)
		self.remove('smith', 1)
		self.check_exists('smith', 1, WT_NOTFOUND)
		for i in xrange(self.NUM_INDICES):
			self.assertEqual(list(self.index_iter(i)), [])
		self.drop_table()

if __name__ == '__main__':
	wttest.run()
