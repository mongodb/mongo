#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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
# test_perf001.py
#	   Regression tests.

import wiredtiger, wttest
import random
import string
from datetime import datetime
from threading import Thread
from time import gmtime, strftime, sleep, time

insert_count = 0
running = 1

def output_progress():
	global insert_count
	global running
	last = 0
	runtime = 0
	print ''
	print '|  Time   | ops | ops/sec | Interval ops | Interval ops/sec |'
	while running:
		sleep(5)
		runtime += 5
		print ('| ' + strftime("%H:%M:%S", gmtime()) +
			' | ' + str(insert_count) +
			' | ' + str(insert_count / runtime) +
			' | ' + str(insert_count - last) +
			' | ' + str((insert_count  - last) / 5) + ' |')
		last = insert_count

# Regression tests.
class test_perf001(wttest.WiredTigerTestCase):
	table_name = 'test_perf001'

	def setUpConnectionOpen(self, dir):
		wtopen_args = 'create,cache_size=512M,statistics_log=(wait=5)'
		#wtopen_args += ',statistics_log=(wait=20)'
		conn = wiredtiger.wiredtiger_open(dir, wtopen_args)
		self.pr(`conn`)
		return conn

	def insert_one(self, c, k1, v1, v2):
		c.set_key(k1)
		c.set_value(v1, v2)
		self.assertEqual(c.insert(), 0)

	def test_performance_of_indeces(self):
		global insert_count
		global running
		thread = Thread(target = output_progress)
		thread.start()
		uri = 'table:' + self.table_name
		create_args = 'key_format=i,value_format=ii,columns=(a,c,d)'
		self.session.create(uri, create_args)
		self.session.create('index:' + self.table_name + ':ia', 'columns=(d,c)')

		c = self.session.open_cursor('table:' + self.table_name, None, None)
		for i in xrange(10000000):
			self.insert_one(c, i, int(time()), random.randint(1,5))
			insert_count += 1
		c.close()
		# Notify the monitoring thread to shut down.
		running = 0
		thread.join()

if __name__ == '__main__':
	wttest.run()

