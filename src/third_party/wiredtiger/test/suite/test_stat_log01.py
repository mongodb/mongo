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

import glob
import os.path
import time
import helper, wiredtiger, wttest
from wiredtiger import stat

# test_stat_log01.py
#    Statistics log
class test_stat_log01(wttest.WiredTigerTestCase):
    """
    Test statistics log
    """

    # Tests need to setup the connection in their own way.
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def test_stats_log_default(self):
        self.conn = self.wiredtiger_open(
            None, "create,statistics=(fast),statistics_log=(wait=1)")
        # Wait for the default interval, to ensure stats have been written.
        time.sleep(2)
        self.check_stats_file(".")

    def test_stats_log_name(self):
        os.mkdir("foo")
        self.conn = self.wiredtiger_open(
            None, "create,statistics=(fast),statistics_log=(wait=1,path=foo)")
        # Wait for the default interval, to ensure stats have been written.
        time.sleep(2)
        self.check_stats_file("foo")

    def test_stats_log_on_close_and_log(self):
        self.conn = self.wiredtiger_open(None,
            "create,statistics=(fast),statistics_log=(on_close=true,wait=1)")
        # Wait for the default interval, to ensure stats have been written.
        time.sleep(2)
        self.close_conn()
        self.check_stats_file(".")

    def test_stats_log_on_close(self):
        self.conn = self.wiredtiger_open(None,
            "create,statistics=(fast),statistics_log=(on_close=true)")
        # Close the connection to ensure the statistics get generated.
        self.close_conn()
        self.check_stats_file(".")

    def check_stats_file(self, dir):
        files = glob.glob(dir + '/' + 'WiredTigerStat.[0-9]*')
        self.assertTrue(files)

if __name__ == '__main__':
    wttest.run()
