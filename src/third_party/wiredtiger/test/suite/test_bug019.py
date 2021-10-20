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

import fnmatch, os, time
import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

# test_bug019.py
#    Test that pre-allocating log files only pre-allocates a small number.
class test_bug019(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled,file_max=100K),statistics=(fast)'
    uri = "table:bug019"
    entries = 5000
    max_prealloc = 1

    # Modify rows so we write log records. We're writing a lot more than a
    # single log file, so we know the underlying library will churn through
    # log files.
    def get_prealloc_stat(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prealloc = stat_cursor[stat.conn.log_prealloc_max][2]
        stat_cursor.close()
        return prealloc

    def populate(self, nentries):
        c = self.session.open_cursor(self.uri, None, None)
        for i in range(0, nentries):
            # Make the values about 200 bytes. That's about 1MB of data for
            # 5000 records, generating 10 log files used plus more for overhead.
            c[i] = "abcde" * 40
            if i % 500 == 0:
                prealloc = self.get_prealloc_stat()
                if prealloc > self.max_prealloc:
                    self.max_prealloc = prealloc
        c.close()

    # Wait for a log file to be pre-allocated. Avoid timing problems, but
    # assert a file is created within 30 seconds.
    def prepfiles(self):
        for i in range(1,30):
                f = fnmatch.filter(os.listdir('.'), "*Prep*")
                if f:
                        return f
                time.sleep(1.0)
        self.assertFalse(not f)

    # There was a bug where pre-allocated log files accumulated on
    # Windows systems due to an issue with the directory list code.
    def test_bug019(self):
        # Create a table just to write something into the log.
        self.session.create(self.uri, 'key_format=i,value_format=S')
        start_prealloc = self.get_prealloc_stat()
        self.populate(self.entries)
        self.session.checkpoint()
        self.assertTrue(self.max_prealloc > start_prealloc)

        # Loop, making sure pre-allocation is working and the range is moving.
        older = self.prepfiles()
        for i in range(1, 10):
            self.populate(self.entries)
            newer = self.prepfiles()

            # Files can be returned in any order when reading a directory, older
            # pre-allocated files can persist longer than newer files when newer
            # files are returned first. Confirm files are being consumed.
            self.assertFalse(set(older) < set(newer))

            older = newer
            self.session.checkpoint()

        # Wait for a long time for pre-allocate to drop in an idle system
        # it should usually be fast, but on slow systems can take time.
        max_wait_time = 90
        for sleepcount in range(1,max_wait_time):
            new_prealloc = self.get_prealloc_stat()
            if new_prealloc < self.max_prealloc:
                break
            time.sleep(1.0)
        self.assertTrue(sleepcount < max_wait_time)

if __name__ == '__main__':
    wttest.run()
