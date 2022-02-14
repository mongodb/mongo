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

import os, shutil
import wttest
from wiredtiger import stat

# test_log03.py
#    test configuration for log.dirty_max
class test_log03(wttest.WiredTigerTestCase):
    """
    Test log.dirty_max
    """

    homedir = 'HOME'
    uri = 'table:test_log03'
    nentries = 20000

    # Tests need to setup the connection in their own way.
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def populate(self):
        big_str = 'A' * 10000
        self.session.create(self.uri, "key_format=S,value_format=S")
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nentries):
            cursor[str(i)] = big_str
        cursor.close()

    def fsync_stat(self):
        cursor = self.session.open_cursor('statistics:', None, None)
        result = cursor[stat.conn.fsync_io][2]
        cursor.close()
        return result

    def with_log_sync(self, log_size, dirty_pct):
        config = "cache_size=1G,create,statistics=(fast),log=(enabled"
        config += ",file_max=" + str(log_size) + "M"
        config += ",os_cache_dirty_pct=" + str(dirty_pct)
        config += "),transaction_sync=(enabled=false,method=none)"
        #self.tty('CONFIG: ' + config)

        # Recreate a home directory each time so we have the log
        # starting at zero.  That makes our calculations easier.
        shutil.rmtree(self.homedir, ignore_errors=True)
        os.mkdir(self.homedir)
        self.conn = self.wiredtiger_open(self.homedir, config)
        self.session = self.conn.open_session(None)
        self.populate()
        result = self.fsync_stat()
        self.session.close()
        self.conn.close()
        return result

    def test_dirty_max(self):
        # With this test, we have a baseline of syncs performed for 12M
        # log files.  Then we set dirty_max to values that are half,
        # a third, a quarter and a fifth of the log file, and we would
        # expect an increase of syncs each time.  The number of syncs
        # produced turns out to be a little variable, so we've picked
        # conservative increases.
        baseline = self.with_log_sync(12, 0)
        #self.tty('baseline: ' + str(baseline))

        incr = 5
        for dirty_pct,increase in [50, incr], [33, incr*2], [25, incr*3], [20, incr*4]:
            result = self.with_log_sync(12, dirty_pct)
            #self.tty('tried: ' + str(dirty_pct) + ', got: ' + str(result) + ', expected: ' + str(baseline + increase))
            self.assertGreater(result, baseline + increase)

if __name__ == '__main__':
    wttest.run()
