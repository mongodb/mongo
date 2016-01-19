#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
#
# test_nsnap02.py
#   Named snapshots: Combinations of dropping snapshots

from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios
from helper import simple_populate
import wiredtiger, wttest

class test_nsnap02(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_nsnap02'
    uri = 'table:' + tablename
    nrows = 1000
    nrows_per_snap = 10
    nsnapshots = 10

    def check_named_snapshot(self, c, snapshot, expected):
        c.reset()
        self.session.begin_transaction("snapshot=" + str(snapshot))
        count = 0
        for row in c:
            count += 1
        self.session.commit_transaction()
        # print "Checking snapshot %d, expect %d, found %d" % (snapshot, expected, count)
        self.assertEqual(count, expected)

    def check_named_snapshots(self, snapshots):
        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            if dropped == 0:
                self.check_named_snapshot(c, snap_name, expected)
            else:
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                    self.session.begin_transaction("snapshot=%d" % (snap_name)),
                    "/Invalid argument/")

    def create_snapshots(self):
        # Populate a table
        end = start = 0
        simple_populate(self, self.uri, 'key_format=i', 0)

        # Create a set of snapshots
        # Each snapshot has a bunch of new data
        # Each snapshot removes a (smaller) bunch of old data
        snapshots = []
        c = self.session.open_cursor(self.uri)
        for n in xrange(self.nsnapshots):
            self.session.snapshot("name=%d" % (n))
            snapshots.append((n, end - start, 0))
            for i in xrange(2 * self.nrows_per_snap):
                c[end + i] = "some value"
            end += 2 * self.nrows_per_snap
            for i in xrange(self.nrows_per_snap):
                del c[start + i]
            start += self.nrows_per_snap
        return snapshots

    def test_drop_all_snapshots(self):
        snapshots = self.create_snapshots()

        self.check_named_snapshots(snapshots)

        self.session.snapshot("drop=(all)")

        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            new_snapshots.append((snap_name, expected, 1))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_first_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(names=[0])")

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name == 0:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_to_first_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(to=0)")

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name == 0:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_before_first_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(before=0)")

        # Make sure no snapshots are gone
        self.check_named_snapshots(snapshots)

    def test_drop_to_third_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(to=3)")

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name <= 3:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_before_third_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(before=3)")

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name < 3:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_to_last_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(to=%d)" % (self.nsnapshots - 1))

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            new_snapshots.append((snap_name, expected, 1))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_before_last_snapshot(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot("drop=(before=%d)" % (self.nsnapshots - 1))

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name < self.nsnapshots - 1:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

    def test_drop_specific_snapshots1(self):
        snapshots = self.create_snapshots()

        c = self.session.open_cursor(self.uri)
        for snap_name, expected, dropped in snapshots:
            self.check_named_snapshot(c, snap_name, expected)

        self.session.snapshot(
            "drop=(names=[%d,%d,%d])" % (0, 3, self.nsnapshots - 1))

        # Construct a snapshot array matching the expected state.
        new_snapshots = []
        for snap_name, expected, dropped in snapshots:
            if snap_name == 0 or snap_name == 3 or \
                snap_name == self.nsnapshots - 1:
                new_snapshots.append((snap_name, expected, 1))
            else:
                new_snapshots.append((snap_name, expected, 0))

        # Make sure all the snapshots are gone.
        self.check_named_snapshots(new_snapshots)

if __name__ == '__main__':
    wttest.run()
