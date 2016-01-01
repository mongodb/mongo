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
# test_nsnap03.py
#   Named snapshots: Access and create from multiple sessions

from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios
from helper import simple_populate
import wiredtiger, wttest

class test_nsnap03(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_nsnap03'
    uri = 'table:' + tablename
    nrows = 300
    nrows_per_snap = 10
    nsnapshots = 10

    def check_named_snapshot(self, snapshot, expected):
        new_session = self.conn.open_session()
        new_session.begin_transaction("snapshot=" + str(snapshot))
        c = new_session.open_cursor(self.uri)
        count = 0
        for row in c:
            count += 1
        new_session.commit_transaction()
        # print "Checking snapshot %d, expect %d, found %d" % (snapshot, expected, count)
        self.assertEqual(count, expected)
        new_session.close()

    def test_named_snapshots(self):
        # Populate a table
        end = start = 0
        simple_populate(self, self.uri, 'key_format=i', 0)

        # Now run a workload:
        # every iteration:
        # create a new named snapshot, N
        # append 20 rows and delete the first 10
        # verify that every snapshot N contains the expected number of rows
        # if there are more than 10 snapshots active, drop the first half
        snapshots = []
        c = self.session.open_cursor(self.uri)
        for n in xrange(self.nrows / self.nrows_per_snap):
            if len(snapshots) > self.nsnapshots:
                middle = len(snapshots) / 2
                dropcfg = ",drop=(to=%d)" % snapshots[middle][0]
                snapshots = snapshots[middle + 1:]
            else:
                dropcfg = ""

            # Close and start a new session every three snapshots
            if n % 3 == 0:
                self.session.close()
                self.session = self.conn.open_session()
                c = self.session.open_cursor(self.uri)

            self.session.snapshot("name=%d%s" % (n, dropcfg))
            snapshots.append((n, end - start))
            for i in xrange(2 * self.nrows_per_snap):
                c[end + i] = "some value"
            end += 2 * self.nrows_per_snap
            for i in xrange(self.nrows_per_snap):
                del c[start + i]
            start += self.nrows_per_snap

            for snapshot, expected in snapshots:
                self.check_named_snapshot(snapshot, expected)

if __name__ == '__main__':
    wttest.run()
