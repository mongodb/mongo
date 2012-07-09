"""
Python versino of snapshot tests(http://source.wiredtiger.com/1.2.2/snapshots.html). 
We define range of data for different snapshots in snapshots dict. First, we set on to 
build snapsots using build_file_with_snapshots() and then we verify snapshots data inside
check(). Then we test cursor locks and finally we drop the snapshots.
"""
import os
import sys
import traceback
import unittest
import wiredtiger
import wttest

class CheckpointTest(wttest.WiredTigerTestCase):
    checkpoints = {
            "checkpoint-1": (100, 200),
            "checkpoint-2": (200, 220),
            "checkpoint-3": (300, 320),
            "checkpoint-4": (400, 420),
            "checkpoint-5": (500, 520),
            "checkpoint-6": (100, 620),
            "checkpoint-7": (200, 720),
            "checkpoint-8": (300, 820),
            "checkpoint-9": (400, 920)
            }
    URI = "file:__checkpoint"
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, "create, cache_size=100MB")
        self.pr('conn')
        return conn
    def create_session(self):
        config = "key_format=S, value_format=S, internal_page_max=512, leaf_page_max=512"
        self.session.create(self.URI, config)

    def test_checkpoint(self):
        self.create_session()
        self.build_file_with_checkpoints()
        for checkpoint_name, size in self.checkpoints.iteritems():
            self.check(checkpoint_name)
        self.cursor_lock()
        self.drop()
    def build_file_with_checkpoints(self):
        for checkpoint_name, sizes in self.checkpoints.iteritems():
            start, stop = sizes
            self.add_records(start, stop)
            self.session.checkpoint("name=%s" % checkpoint_name)
    
    def add_records(self, start, stop):
        cursor = self.session.open_cursor(self.URI, None, "overwrite")
        for i in range(start, stop+1):
            kbuf = "%010d KEY------" % i
            cursor.set_key(kbuf)
            vbuf = "%010d VALUE----" % i
            cursor.set_value(vbuf)
            result = cursor.insert()
            if result != 0:
                self.fail("cursor.insert(): %s" % result)
        cursor.close()

            
    def dump_records(self, checkpoint_name, filename):
        records = []
        for checkpoint, sizes in self.checkpoints.iteritems():
            sizes = self.checkpoints[checkpoint]
            start, stop = sizes
            for i in range(start, stop+1):
                    records.append("%010d KEY------\n%010d VALUE----\n" % (i, i))
            if checkpoint == checkpoint_name:
                break;
        return records.sort()

    def check(self, checkpoint_name):
        records = self.dump_records(checkpoint_name, "__dump.1")
        snaps = self.dump_snap(checkpoint_name, "__dump.2")
        self.assertEqual(records, snaps)

    def dump_snap(self, checkpoint_name, filename):
        snaps = []
        buf = "checkpoint=%s" % checkpoint_name
        cursor = self.session.open_cursor(self.URI, None, buf)
        while cursor.next() == 0:
            key =  cursor.get_key()
            value = cursor.get_value()
            snaps.append( "%s\n%s\n" % (key, value))
        cursor.close()
        return snaps.sort()
    def cursor_lock(self):
        #self.session.checkpoint()
        #cursor = self.session.open_cursor(self.URI, None, "checkpoint=checkpoint-1")

        #self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
        #        lambda: self.session.checkpoint(),
        #        "session.checkpoint: file:__snap: Resource or device busy")
        #cursor.close()
        self.session.checkpoint("name=another_checkpoint")
        self.session.checkpoint("drop=(from=another_checkpoint)")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(self.URI, None, "checkpoint=another_checkpoint"),
                r'/(.*)no "another_checkpoint" checkpoint found'+\
                            ' in file:__checkpoint:.*/')
        cursor = self.session.open_cursor(self.URI, None, "checkpoint=checkpoint-1")
        #self.session.checkpoint("drop=(from=all)")
        #self.assertRaisesWithMessage(wiredtiger.WiredTigerError, 
                #lambda: self.session.checkpoint("drop=(from=all)"),
                #"/Device or resource busy/")
        cursor.close() 
    def cursor_lock_deprecated(self):
        buf = 'snapshot=snapshot-1'
        cursor = self.session.open_cursor(self.URI, None, buf)
        with self.assertRaises(wiredtiger.WiredTigerError) as cm:
            self.session.drop(self.URI, buf)
        self.assertEqual(0, cursor.close())
        cursor1 = self.session.open_cursor(self.URI, None, buf)
        assert cursor1 != None
        buf = 'snapshot=snapshot-2' 
        cursor2 = self.session.open_cursor(self.URI, None, buf)
        cursor3 = self.session.open_cursor(self.URI, None, None)
        self.assertEqual(0, cursor1.close())
        self.assertEqual(0, cursor2.close())
        self.assertEqual(0, cursor3.close())

    def drop(self):
        self.assertEqual(0, self.session.checkpoint("drop=(from=all)"))
            #self.assertEqual(0, self.session.verify(self.URI, None))
