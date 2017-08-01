#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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

import glob
import json
import os.path
import time
import helper, wiredtiger, wttest
from wiredtiger import stat

# test_stat_log02.py
#    Statistics log JSON testing
class test_stat_log02(wttest.WiredTigerTestCase):
    """
    Test statistics log JSON outputs
    """

    # Tests need to setup the connection in their own way.
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def test_stats_log_json(self):
        self.conn = self.wiredtiger_open(
            None, "create,statistics=(fast),statistics_log=(wait=1,json)")

        self.wait_for_stats_file(".")
        self.check_stats_file(".")

    def test_stats_log_on_json_with_tables(self):
        self.conn = self.wiredtiger_open(None,
            "create,statistics=(fast),statistics_log=(wait=1,json,sources=[file:])")

        # Create a session and table to give us some stats
        session = self.conn.open_session()
        session.create("table:foo")
        c = session.open_cursor("table:foo")
        c["foo"] = "foo"
        c.close()
        session.close()

        self.wait_for_stats_file(".")
        self.close_conn()
        self.check_stats_file(".")
        self.check_file_contains_tables(".")

    def wait_for_stats_file(self, dir):
        # We wait for 30 sleeps then fail
        number_sleeps = 0
        while True:
            time.sleep(1)
            files = glob.glob(dir + '/' + 'WiredTigerStat.[0-9]*')
            for f in files:
                if os.stat(f).st_size != 0:
                    return

            number_sleeps += 1
            self.assertLess(number_sleeps, 30)

    def check_stats_file(self, dir):
        files = glob.glob(dir + '/' + 'WiredTigerStat.[0-9]*')
        self.assertTrue(files)
        self.check_file_is_json(files[0])

    def check_file_is_json(self, file_name):
        f = open(file_name, 'r')
        for line in f:
            # This will throw assertions if we don't have correctly formed JSON
            json.loads(line)

    def check_file_contains_tables(self, dir):
        # We wait for another 30 sleeps here to avoid erroring in the case where
        # the stat log has only made the first pass and not yet printed the
        # individual table stats.
        number_sleeps = 0
        while True:
            files = glob.glob(dir + '/' + 'WiredTigerStat.[0-9]*')
            f = open(files[0], 'r')
            for line in f:
                data = json.loads(line)
                if "wiredTigerTables" in data:
                    if "file:foo.wt" in data["wiredTigerTables"]:
                        return

            number_sleeps += 1
            self.assertLess(number_sleeps, 30)

if __name__ == '__main__':
    wttest.run()
