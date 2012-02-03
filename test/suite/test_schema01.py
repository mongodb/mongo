#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
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

	def cursor(self, config=None):
		self.pr('open cursor')
		return self.session.open_cursor(self.tablename, None, config)

	def test_populate(self):
		'''Populate a table'''
		for reopen in (False, True):
			self.create_table()
			c = self.cursor('overwrite')
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
