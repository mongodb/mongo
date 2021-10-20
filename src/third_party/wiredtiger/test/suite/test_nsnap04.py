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
#
# test_nsnap04.py
#   Named snapshots: Create snapshot from running transaction

import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet

class test_nsnap04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_nsnap04'
    uri = 'table:' + tablename
    nrows_per_itr = 10

    def check_named_snapshot(self, snapshot, expected, skip_snapshot=False):
        new_session = self.conn.open_session()
        c = new_session.open_cursor(self.uri)
        if skip_snapshot:
            new_session.begin_transaction()
        else:
            new_session.begin_transaction("snapshot=" + str(snapshot))
        count = 0
        for row in c:
            count += 1
        new_session.commit_transaction()
        new_session.close()
        # print "Checking snapshot %d, expect %d, found %d" % (snapshot, expected, count)
        self.assertEqual(count, expected)

    def test_named_snapshots(self):
        # Populate a table
        end = start = 0
        SimpleDataSet(self, self.uri, 0, key_format='i').populate()

        snapshots = []
        c = self.session.open_cursor(self.uri)
        for i in xrange(self.nrows_per_itr):
            c[i] = "some value"

        # Start a new transaction in a different session
        new_session = self.conn.open_session()
        new_session.begin_transaction("isolation=snapshot")
        new_c = new_session.open_cursor(self.uri)
        count = 0
        for row in new_c:
            count += 1
        new_session.snapshot("name=0")

        self.check_named_snapshot(0, self.nrows_per_itr)

        # Insert some more content using the original session.
        for i in xrange(self.nrows_per_itr):
            c[2 * self.nrows_per_itr + i] = "some value"

        self.check_named_snapshot(0, self.nrows_per_itr)
        new_session.close()
        # Update the named snapshot
        self.session.snapshot("name=0")
        self.check_named_snapshot(0, 2 * self.nrows_per_itr)

    def test_include_updates(self):
        # Populate a table
        end = start = 0
        SimpleDataSet(self, self.uri, 0, key_format='i').populate()

        snapshots = []
        c = self.session.open_cursor(self.uri)
        for i in xrange(self.nrows_per_itr):
            c[i] = "some value"

        self.session.begin_transaction("isolation=snapshot")
        count = 0
        for row in c:
            count += 1
        self.session.snapshot("name=0,include_updates=true")

        self.check_named_snapshot(0, self.nrows_per_itr)

        # Insert some more content using the active session.
        for i in xrange(self.nrows_per_itr):
            c[self.nrows_per_itr + i] = "some value"

        self.check_named_snapshot(0, 2 * self.nrows_per_itr)
        # Ensure transactions not tracking the snapshot don't see the updates
        self.check_named_snapshot(0, self.nrows_per_itr, skip_snapshot=True)
        self.session.commit_transaction()
        # Ensure content is visible to non-snapshot transactions after commit
        self.check_named_snapshot(0, 2 * self.nrows_per_itr, skip_snapshot=True)

if __name__ == '__main__':
    wttest.run()
