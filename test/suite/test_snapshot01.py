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
from collections import OrderedDict

class SnapShotTest(wttest.WiredTigerTestCase):

    snapshots = {
        "snapshot-1": (100, 120),
        "snapshot-2": (200, 220),
        "snapshot-3": (300, 320),
        "snapshot-4": (400, 420),
        "snapshot-5": (500, 520),
        "snapshot-6": (100, 620),
        "snapshot-7": (200, 720),
        "snapshot-8": (300, 820),
        "snapshot-9": (400, 920)
    }
    snapshots = OrderedDict(sorted(snapshots.items(), key=lambda t: t[0]))
    URI = "file:__snap"
    
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, "create, cache_size=100MB")
        self.pr('conn')
        return conn
    def create_session(self):
        config = "key_format=S, value_format=S, internal_page_max=512, leaf_page_max=512"
        self.session.create(self.URI, config)

    def test_snapshot(self):
        self.create_session()
        self.build_file_with_snapshots() # build set of snapshots
        for snapshot_names, sizes in self.snapshots.iteritems():
            self.check(snapshot_names)
        self.cursor_lock()
        self.drop()

    def build_file_with_snapshots(self):
        for snapshot_name, sizes in self.snapshots.iteritems():
            start, stop = sizes
            self.add_records(start, stop)
            buf = "snapshot=%s" % snapshot_name
            self.assertEqual(0, self.session.sync(self.URI, buf))
            self.assertEqual(0, self.session.verify(self.URI, None))
    
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

            
    def dump_records(self, snapshot_name, filename):
        records = []
        for snapshot, sizes in self.snapshots.iteritems():
            sizes = self.snapshots[snapshot]
            start, stop = sizes
            for i in range(start, stop+1):
                    records.append("%010d KEY------\n%010d VALUE----\n" % (i, i))
            if snapshot == snapshot_name:
                break;
        return records.sort()

    def check(self, snapshot_name):
        records = self.dump_records(snapshot_name, "__dump.1")
        snaps = self.dump_snap(snapshot_name, "__dump.2")
        self.assertEqual(records, snaps)

    def dump_snap(self, snapshot_name, filename):
        snaps = []
        buf = "snapshot=%s" % snapshot_name
        cursor = self.session.open_cursor(self.URI, None, buf)
        while cursor.next() == 0:
            key =  cursor.get_key()
            value = cursor.get_value()
            snaps.append( "%s\n%s\n" % (key, value))
        cursor.close()
        return snaps.sort()

    def cursor_lock(self):
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
        for snapshot_name, sizes in self.snapshots.iteritems():
            start, stop = sizes
            buf = 'snapshot=%s' % snapshot_name
            self.assertEqual(0, self.session.drop(self.URI, buf))
            self.assertEqual(0, self.session.verify(self.URI, None))
