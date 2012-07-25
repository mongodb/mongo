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
# test_checkpoint01.py
# 	Checkpoint tests
#
# We define a range of data for different snapshots in snapshots dict.
# First, we build the snapsots using build_file_with_snapshots(), then we
# verify snapshots data inside check(). Then we test cursor locks and finally
# we drop the snapshots.

import wiredtiger, wttest

class test_checkpoint(wttest.WiredTigerTestCase):
    checkpoints = {
	"checkpoint-1": (100, 200),
	"checkpoint-2": (200, 220),
	"checkpoint-3": (300, 320),
	"checkpoint-4": (400, 420),
	"checkpoint-5": (500, 520),
	"checkpoint-6": (100, 620),
	"checkpoint-7": (200, 250),
	"checkpoint-8": (300, 820),
	"checkpoint-9": (400, 920)
	}
    URI = "file:__checkpoint"
    
    # Add a set of records for a checkpoint.
    def add_records(self, name, start, stop):
	cursor = self.session.open_cursor(self.URI, None, "overwrite")
	for i in range(start, stop+1):
	    cursor.set_key("%010d KEY------" % i)
	    cursor.set_value("%010d VALUE "% i + name)
	    self.assertEqual(cursor.insert(), 0)
	cursor.close()

    # For each checkpoint entry, add/overwrite the specified records, then
    # checkpoint the object, and verify it (which verifies all underlying
    # checkpoints individually).
    def build_file_with_checkpoints(self):
	for checkpoint_name, sizes in self.checkpoints.iteritems():
	    start, stop = sizes
	    self.add_records(checkpoint_name, start, stop)
	    self.session.checkpoint("name=" + checkpoint_name)
	    self.session.verify(self.URI, None)
	    
    # Create a dictionary of sorted records a checkpoint should include.
    def list_expected(self, name):
	records = {}
	for checkpoint_name, sizes in self.checkpoints.iteritems():
	    start, stop = sizes
	    for i in range(start, stop+1):
		records['%010d KEY------' % i] =\
		    '%010d VALUE ' % i + checkpoint_name
	    if name == checkpoint_name:
		break;
	return records

    # Create a dictionary of sorted records a checkpoint does include.
    def list_checkpoint(self, name):
	records = {}
	cursor = self.session.open_cursor(self.URI, None, 'checkpoint=' + name)
	while cursor.next() == 0:
	    records[cursor.get_key()] = cursor.get_value()
	cursor.close()
	return records

    # For each checkpoint entry, verify it contains the records it should.
    def check(self):
	for checkpoint_name, sizes in self.checkpoints.iteritems():
	    list_expected = self.list_expected(checkpoint_name)
	    list_checkpoint = self.list_checkpoint(checkpoint_name)
	    self.assertEqual(list_expected, list_checkpoint)

    def test_checkpoint(self):
	config = "key_format=S,value_format=S,leaf_page_max=512"
	self.session.create(self.URI, config)
	self.build_file_with_checkpoints()
	self.check()
	self.cursor_lock()
	self.drop()

    def cursor_lock(self):
	self.session.checkpoint("name=another_checkpoint")
	self.session.checkpoint("drop=(from=another_checkpoint)")
	self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
	    lambda: self.session.open_cursor(
	    self.URI, None, "checkpoint=another_checkpoint"),
		r'/(.*)no "another_checkpoint" checkpoint found'+\
		' in file:__checkpoint:.*/')
	
	#cursor = self.session.open_cursor(
	#   self.URI, None, 'checkpoint=checkpoint-1')
	#self.session.checkpoint()
	#cursor.close()

	cursor = self.session.open_cursor(
	    self.URI, None, "checkpoint=checkpoint-1")
	#self.session.checkpoint("drop=(from=checkpoint-1)")
	self.assertRaisesWithMessage(wiredtiger.WiredTigerError, 
	    lambda: self.session.checkpoint("drop=(from=checkpoint-1)"), r"")
	cursor.close() 

    def drop(self):
	self.session.checkpoint("drop=(from=all)")
	self.session.verify(self.URI, None)
