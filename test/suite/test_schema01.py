#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_schema01.py
#	Test that tables are reconciled correctly when they are empty.
#

import unittest
import wiredtiger
from wiredtiger import WT_NOTFOUND
import wttest

pop_data = [
	( 'USA', 1980, 226542250 ),
	( 'USA', 2009, 307006550 ),
	( 'UK', 2008, 61414062 ),
	( 'CAN', 2008, 33311400 ),
	( 'AU', 2008, 21431800 )
]

class test_schema01(wttest.WiredTigerTestCase):
	'''Test various tree types becoming empty'''

	basename = 'test_schema01'
	tablename = 'table:' + basename
	cgname = 'colgroup:' + basename

	def __init__(self, *args, **kwargs):
		wttest.WiredTigerTestCase.__init__(self, *args, **kwargs)
		self.reconcile = False

	def reopen(self):
		self.conn.close()
		self.conn = wiredtiger.wiredtiger_open('.', None)
		self.session = self.conn.open_session()

	def create_table(self):
		self.pr('create table')
		self.session.create(self.tablename, 'key_format=5s,value_format=HQ,' +
				'columns=(country,year,population),' +
				'colgroups=(year,population)')
		self.session.create(self.cgname + ':year', 'columns=(year)')
		self.session.create(self.cgname + ':population', 'columns=(population)')

	def drop_table(self):
		self.pr('drop table')
		self.session.drop(self.tablename)

	def cursor(self):
		self.pr('open cursor')
		return self.session.open_cursor(self.tablename, None)

	def test_populate(self):
		'''Populate a table'''
		for reopen in (False, True):
			self.create_table()
			c = self.cursor()
			try:
				for record in pop_data:
					c.set_key(record[0])
					c.set_value(*record[1:])
					c.insert()
			finally:
				c.close()

			if reopen:
				self.reopen()

			c = self.cursor()
			for record in c:
				print record
			c.close()
			self.drop_table()

if __name__ == '__main__':
	wttest.run()
