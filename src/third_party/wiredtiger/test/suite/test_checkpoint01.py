#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
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

import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_checkpoint01.py
#    Checkpoint tests
# General checkpoint test: create an object containing sets of data associated
# with a set of checkpoints, then confirm the checkpoint's values are correct,
# including after other checkpoints are dropped.
class test_checkpoint(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file', dict(uri='file:checkpoint',fmt='S')),
        ('table', dict(uri='table:checkpoint',fmt='S'))
    ])

    # Each checkpoint has a key range and a "is dropped" flag.
    checkpoints = {
        "checkpoint-1": ((100, 200), 0),
        "checkpoint-2": ((200, 220), 0),
        "checkpoint-3": ((300, 320), 0),
        "checkpoint-4": ((400, 420), 0),
        "checkpoint-5": ((500, 520), 0),
        "checkpoint-6": ((100, 620), 0),
        "checkpoint-7": ((200, 250), 0),
        "checkpoint-8": ((300, 820), 0),
        "checkpoint-9": ((400, 920), 0)
        }

    # Add a set of records for a checkpoint.
    def add_records(self, name):
        cursor = self.session.open_cursor(self.uri, None, "overwrite")
        start, stop = self.checkpoints[name][0]
        for i in range(start, stop+1):
            cursor["%010d KEY------" % i] = ("%010d VALUE " % i) + name
        cursor.close()
        self.checkpoints[name] = (self.checkpoints[name][0], 1)

    # For each checkpoint entry, add/overwrite the specified records, then
    # checkpoint the object, and verify it (which verifies all underlying
    # checkpoints individually).
    def build_file_with_checkpoints(self):
        for checkpoint_name, entry in self.checkpoints.items():
            self.add_records(checkpoint_name)
            self.session.checkpoint("name=" + checkpoint_name)

    # Create a dictionary of sorted records a checkpoint should include.
    def list_expected(self, name):
        records = {}
        for checkpoint_name, entry in self.checkpoints.items():
            start, stop = entry[0]
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

    # For each existing checkpoint entry, verify it contains the records it
    # should, and no other checkpoints exist.
    def check(self):
        # Physically verify the file, including the individual checkpoints.
        self.session.verify(self.uri, None)

        for checkpoint_name, entry in self.checkpoints.items():
            if entry[1] == 0:
                self.assertRaises(wiredtiger.WiredTigerError,
                    lambda: self.session.open_cursor(
                    self.uri, None, "checkpoint=" + checkpoint_name))
            else:
                list_expected = self.list_expected(checkpoint_name)
                list_checkpoint = self.list_checkpoint(checkpoint_name)
                self.assertEqual(list_expected, list_checkpoint)

    # Main checkpoint test driver.
    def test_checkpoint(self):
        # Build a file with a set of checkpoints, and confirm they all have
        # the correct key/value pairs.
        self.session.create(self.uri,
            "key_format=" + self.fmt +\
                ",value_format=S,allocation_size=512,leaf_page_max=512")
        self.build_file_with_checkpoints()
        self.check()

        # Drop a set of checkpoints sequentially, and each time confirm the
        # contents of remaining checkpoints, and that dropped checkpoints
        # don't appear.
        for i in [1,3,7,9]:
            checkpoint_name = 'checkpoint-' + str(i)
            self.session.checkpoint('drop=(' + checkpoint_name + ')')
            self.checkpoints[checkpoint_name] =\
                (self.checkpoints[checkpoint_name][0], 0)
            self.check()

        # Drop remaining checkpoints, all subsequent checkpoint opens should
        # fail.
        self.session.checkpoint("drop=(from=all)")
        for checkpoint_name, entry in self.checkpoints.items():
            self.checkpoints[checkpoint_name] =\
                (self.checkpoints[checkpoint_name][0], 0)
        self.check()

# Check some specific cursor checkpoint combinations.
class test_checkpoint_cursor(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file', dict(uri='file:checkpoint',fmt='S')),
        ('table', dict(uri='table:checkpoint',fmt='S'))
    ])

    # Check that you cannot open a checkpoint that doesn't exist.
    def test_checkpoint_dne(self):
        SimpleDataSet(self, self.uri, 100, key_format=self.fmt).populate()
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.uri, None, "checkpoint=checkpoint-1"))
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint"))

    # Check that you can open checkpoints more than once.
    def test_checkpoint_multiple_open(self):
        SimpleDataSet(self, self.uri, 100, key_format=self.fmt).populate()
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
        SimpleDataSet(self, self.uri, 100, key_format=self.fmt).populate()
        self.session.checkpoint("name=checkpoint-1")
        self.session.checkpoint("name=checkpoint-2")
        self.session.checkpoint("name=checkpoint-3")
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=checkpoint-2")

        msg = '/checkpoint.*cannot be dropped/'
        # Check creating an identically named checkpoint fails. */
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("force,name=checkpoint-2"), msg)
        # Check dropping the specific checkpoint fails.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("drop=(checkpoint-2)"), msg)
        # Check dropping all checkpoints fails.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("drop=(from=all)"), msg)

        # Check dropping other checkpoints succeeds (which also tests that you
        # can create new checkpoints while other checkpoints are in-use).
        self.session.checkpoint("drop=(checkpoint-1,checkpoint-3)")

        # Close the cursor and repeat the failing commands, they should now
        # succeed.
        cursor.close()
        self.session.checkpoint("name=checkpoint-2")
        self.session.checkpoint("drop=(checkpoint-2)")
        self.session.checkpoint("drop=(from=all)")

# Check that you can checkpoint targets.
class test_checkpoint_target(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file', dict(uri='file:checkpoint',fmt='S')),
        ('table', dict(uri='table:checkpoint',fmt='S'))
    ])

    def update(self, uri, ds, value):
        cursor = ds.open_cursor(uri, None, "overwrite")
        cursor[ds.key(10)] = value
        cursor.close()

    def check(self, uri, ds, value):
        cursor = ds.open_cursor(uri, None, "checkpoint=checkpoint-1")
        self.assertEquals(cursor[ds.key(10)], value)
        cursor.close()

    # FIXME-WT-10836
    @wttest.skip_for_hook("tiered", "strange interaction with tiered and named checkpoints using target")
    def test_checkpoint_target(self):
        # Create 3 objects, change one record to an easily recognizable string.
        uri = self.uri + '1'
        ds1 = SimpleDataSet(self, uri, 100, key_format=self.fmt)
        ds1.populate()
        self.update(uri, ds1, 'ORIGINAL')

        uri = self.uri + '2'
        ds2 = SimpleDataSet(self, uri, 100, key_format=self.fmt)
        ds2.populate()
        self.update(uri, ds2, 'ORIGINAL')

        uri = self.uri + '3'
        ds3 = SimpleDataSet(self, uri, 100, key_format=self.fmt)
        ds3.populate()
        self.update(uri, ds3, 'ORIGINAL')

        # Checkpoint all three objects.
        self.session.checkpoint("name=checkpoint-1")

        # Update all 3 objects, then checkpoint two of the objects with the
        # same checkpoint name.
        self.update(self.uri + '1', ds1, 'UPDATE')
        self.update(self.uri + '2', ds2, 'UPDATE')
        self.update(self.uri + '3', ds3, 'UPDATE')
        target = 'target=("' + self.uri + '1"' + ',"' + self.uri + '2")'
        self.session.checkpoint("name=checkpoint-1," + target)

        # Confirm the checkpoint has the old value in objects that weren't
        # checkpointed, and the new value in objects that were checkpointed.
        self.check(self.uri + '1', ds1, 'UPDATE')
        self.check(self.uri + '2', ds2, 'UPDATE')
        self.check(self.uri + '3', ds3, 'ORIGINAL')

# Check that you can't write checkpoint cursors.
class test_checkpoint_cursor_update(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file-r', dict(uri='file:checkpoint',fmt='r')),
        ('file-S', dict(uri='file:checkpoint',fmt='S')),
        ('table-r', dict(uri='table:checkpoint',fmt='r')),
        ('table-S', dict(uri='table:checkpoint',fmt='S'))
    ])

    def test_checkpoint_cursor_update(self):
        ds = SimpleDataSet(self, self.uri, 100, key_format=self.fmt)
        ds.populate()
        self.session.checkpoint("name=ckpt")
        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")
        cursor.set_key(ds.key(10))
        cursor.set_value("XXX")
        msg = "/Unsupported cursor/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.insert(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.remove(), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.update(), msg)
        cursor.close()

# Check that WiredTigerCheckpoint works as a checkpoint specifier.
class test_checkpoint_last(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file', dict(uri='file:checkpoint',fmt='S')),
        ('table', dict(uri='table:checkpoint',fmt='S'))
    ])

    def test_checkpoint_last(self):
        # Create an object, change one record to an easily recognizable string,
        # then checkpoint it and open a cursor, confirming we see the correct
        # value.   Repeat this action, we want to be sure the engine gets the
        # latest checkpoint information each time.
        uri = self.uri
        ds = SimpleDataSet(self, uri, 100, key_format=self.fmt)
        ds.populate()

        for value in ('FIRST', 'SECOND', 'THIRD', 'FOURTH', 'FIFTH'):
            # Update the object.
            cursor = ds.open_cursor(uri, None, "overwrite")
            cursor[ds.key(10)] = value
            cursor.close()

            # Checkpoint the object.
            self.session.checkpoint()

            # Verify the "last" checkpoint sees the correct value.
            cursor = ds.open_cursor(
                uri, None, "checkpoint=WiredTigerCheckpoint")
            self.assertEquals(cursor[ds.key(10)], value)
            # Don't close the checkpoint cursor, we want it to remain open until
            # the test completes.

# Check we can't use the reserved name as an application checkpoint name or open a checkpoint cursor
# with it.
class test_checkpoint_illegal_name(wttest.WiredTigerTestCase):
    def test_checkpoint_illegal_name(self):
        uri = "file:checkpoint"
        ds = SimpleDataSet(self, uri, 100, key_format='S')
        ds.populate()
        msg = '/the checkpoint name.*is reserved/'
        for conf in (
            'name=WiredTigerCheckpoint',
            'name=WiredTigerCheckpoint.',
            'name=WiredTigerCheckpointX',
            'drop=(from=WiredTigerCheckpoint)',
            'drop=(from=WiredTigerCheckpoint.)',
            'drop=(from=WiredTigerCheckpointX)',
            'drop=(to=WiredTigerCheckpoint)',
            'drop=(to=WiredTigerCheckpoint.)',
            'drop=(to=WiredTigerCheckpointX)'):
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.checkpoint(conf), msg)
        msg = '/WiredTiger objects should not include grouping/'
        for conf in (
            'name=check{point',
            'name=check\\point'):
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.checkpoint(conf), msg)
        msg = '/the prefix.*is reserved/'
        for conf in (
            'checkpoint=WiredTigerCheckpoint.',
            'checkpoint=WiredTigerCheckpointX'):
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.open_cursor(uri, None, conf), msg)
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.open_cursor("file:WiredTigerHS.wt", None, conf), msg)

# Check we can't name checkpoints that include LSM tables.
class test_checkpoint_lsm_name(wttest.WiredTigerTestCase):
    def test_checkpoint_lsm_name(self):
        ds = ComplexLSMDataSet(self, "table:checkpoint", 1000)
        ds.populate()
        msg = '/object does not support named checkpoints/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("name=ckpt"), msg)

class test_checkpoint_empty(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file', dict(uri='file:checkpoint')),
        ('table', dict(uri='table:checkpoint')),
    ])

    # Create an empty file, do one of 4 cases of checkpoint, then verify the
    # checkpoints exist.   The reason for the 4 cases is we must create all
    # checkpoints an application can explicitly reference using a cursor, and
    # I want to test when other types of checkpoints are created before the
    # checkpoint we really need.
    # Case 1: a named checkpoint
    def test_checkpoint_empty_one(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint('name=ckpt')
        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")

    # Case 2: an internal checkpoint
    def test_checkpoint_empty_two(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint()
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")

    # Case 3: a named checkpoint, then an internal checkpoint
    def test_checkpoint_empty_three(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint('name=ckpt')
        self.session.checkpoint()
        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")

    # Case 4: an internal checkpoint, then a named checkpoint
    def test_checkpoint_empty_four(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint()
        self.session.checkpoint('name=ckpt')
        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")

    # Check that we can create an empty checkpoint, change the underlying
    # object, checkpoint again, and still see the original empty tree, for
    # both named and unnamed checkpoints.
    def test_checkpoint_empty_five(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint('name=ckpt')
        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")
        self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None)
        cursor["key"] = "value"
        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, "checkpoint=ckpt")
        self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)

    # Check that if we create an unnamed and then a named checkpoint, opening
    # WiredTigerCheckpoint opens the most recent (the named) checkpoint.
    def test_checkpoint_empty_six(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint()
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")
        self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None)
        cursor["key"] = "value"
        self.session.checkpoint('name=ckpt')

        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")
        self.assertEquals(cursor.next(), 0)

    # Check that if we create a named and then an unnamed checkpoint, opening
    # WiredTigerCheckpoint opens the most recent (the named) checkpoint.
    def test_checkpoint_empty_seven(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.checkpoint('name=ckpt')
        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")
        self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None)
        cursor["key"] = "value"
        self.session.checkpoint()

        cursor = self.session.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")
        self.assertEquals(cursor.next(), 0)

if __name__ == '__main__':
    wttest.run()
