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

import wiredtiger, wttest
from helper import keyPopulate, simplePopulate

# General checkpoint test: create an object containing sets of data associated
# with a set of checkpoints, then confirm the checkpoint's values are correct,
# including after other checkpoints are dropped.
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

    scenarios = [
	('file', dict(uri='file:checkpoint',fmt='S')),
	('table', dict(uri='table:checkpoint',fmt='S'))
	]

    # Add a set of records for a checkpoint.
    def add_records(self, name, start, stop):
	cursor = self.session.open_cursor(self.uri, None, "overwrite")
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
	    self.session.verify(self.uri, None)
	    
    # Create a dictionary of sorted records a checkpoint should include.
    def list_expected(self, name):
	records = {}
	for checkpoint_name, sizes in self.checkpoints.iteritems():
	    start, stop = sizes
	    for i in range(start, stop+1):
		records['%010d KEY------' % i] =\
		    '%010d VALUE ' % i + checkpoint_name
	    if name == checkpoint_name:
		break
	return records

    # Create a dictionary of sorted records a checkpoint does include.
    def list_checkpoint(self, name):
	records = {}
	cursor = self.session.open_cursor(self.uri, None, 'checkpoint=' + name)
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
	self.session.create(self.uri,
	    "key_format=" + self.fmt + ",value_format=S,leaf_page_max=512")
	self.build_file_with_checkpoints()
	self.check()
	self.drop()

    def drop(self):
	self.session.checkpoint("drop=(from=all)")
	self.session.verify(self.uri, None)

# Check some specific cursor checkpoint combinations.
class test_checkpoint_cursor(wttest.WiredTigerTestCase):
    scenarios = [
	('file', dict(uri='file:checkpoint',fmt='S')),
	('table', dict(uri='table:checkpoint',fmt='S'))
	]

    # Check that you cannot open a checkpoint that doesn't exist.
    def test_checkpoint_dne(self):
	simplePopulate(self, self.uri, 'key_format=' + self.fmt, 100)
	msg = '/no.*checkpoint found/'
	self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
	    lambda: self.session.open_cursor(
	    self.uri, None, "checkpoint=checkpoint-1"), msg)

    # Check that you can open checkpoints more than once.
    def test_checkpoint_multiple_open(self):
	simplePopulate(self, self.uri, 'key_format=' + self.fmt, 100)
	self.session.checkpoint("name=checkpoint-1")
	c1 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-1")
	c2 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-1")
	c3 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-1")
	c4 = self.session.open_cursor(self.uri, None, None)
	c4.close, c3.close, c2.close, c1.close

	self.session.checkpoint("name=checkpoint-2")
	c1 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-1")
	c2 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-2")
	c3 = self.session.open_cursor(self.uri, None, "checkpoint=checkpoint-2")
	c4 = self.session.open_cursor(self.uri, None, None)
	c4.close, c3.close, c2.close, c1.close

    # Check that you cannot drop a checkpoint if it's in use.
    def test_checkpoint_inuse(self):
	simplePopulate(self, self.uri, 'key_format=' + self.fmt, 100)
	self.session.checkpoint("name=checkpoint-1")
	self.session.checkpoint("name=checkpoint-2")
	self.session.checkpoint("name=checkpoint-3")
	cursor = self.session.open_cursor(
	    self.uri, None, "checkpoint=checkpoint-2")

	# Check dropping the specific checkpoint fails.
	self.assertRaises(wiredtiger.WiredTigerError,
	    lambda: self.session.checkpoint("drop=(checkpoint-2)"))

	# Check dropping all checkpoints fails.
	self.assertRaises(wiredtiger.WiredTigerError,
	    lambda: self.session.checkpoint("drop=(from=all)"))

	# Check creating an identically named checkpoint fails (because it will
	# attempt to drop the open checkpoint).
	self.assertRaises(wiredtiger.WiredTigerError,
	    lambda: self.session.checkpoint("name=checkpoint-2"))

	# Check dropping other checkpoints succeeds (which also tests that you
	# can create new checkpoints while other checkpoints are in-use).
	self.session.checkpoint("drop=(checkpoint-1,checkpoint-3)")

	# Close the cursor and repeat the failing commands, they should now
	# succeed.
	cursor.close()
	self.session.checkpoint("drop=(checkpoint-2)")
	self.session.checkpoint("drop=(from=all)")

# Check that you can checkpoint targets.
class test_checkpoint_target(wttest.WiredTigerTestCase):
    scenarios = [
	('file', dict(uri='file:checkpoint',fmt='S')),
	('table', dict(uri='table:checkpoint',fmt='S'))
	]

    def update(self, uri, value):
	cursor = self.session.open_cursor(uri, None, "overwrite")
	cursor.set_key(keyPopulate(self.fmt, 10))
	cursor.set_value(value)
	cursor.insert()
	cursor.close()

    def check(self, uri, value):
	cursor = self.session.open_cursor(uri, None, "checkpoint=checkpoint-1")
	cursor.set_key(keyPopulate(self.fmt, 10))
	cursor.search()
	self.assertEquals(cursor.get_value(), value)
	cursor.close()

    def test_checkpoint_target(self):
	# Create 3 objects, change one record to an easily recognizable string
	# and checkpoint them.
	uri = self.uri + '1'
	simplePopulate(self, uri, 'key_format=' + self.fmt, 100)
	self.update(uri, 'ORIGINAL')
	uri = self.uri + '2'
	simplePopulate(self, uri, 'key_format=' + self.fmt, 100)
	self.update(uri, 'ORIGINAL')
	uri = self.uri + '3'
	simplePopulate(self, uri, 'key_format=' + self.fmt, 100)
	self.update(uri, 'ORIGINAL')

	self.session.checkpoint("name=checkpoint-1")

	# Update all 3 objects, then checkpoint two of the objects.
	self.update(self.uri + '1', 'UPDATE')
	self.update(self.uri + '2', 'UPDATE')
	self.update(self.uri + '3', 'UPDATE')
	target = 'target=("' + self.uri + '1"' + ',"' + self.uri + '2")'
	self.session.checkpoint("name=checkpoint-1," + target)

	# Confirm the checkpoint has the old value in objects that weren't
	# checkpointed, and the new value in objects that were checkpointed.
	self.check(self.uri + '1', 'UPDATE')
	self.check(self.uri + '2', 'UPDATE')
	self.check(self.uri + '3', 'ORIGINAL')
