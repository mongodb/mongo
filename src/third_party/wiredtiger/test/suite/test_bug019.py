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
import wttest
from wiredtiger import stat

# test_bug019.py
#    Test that pre-allocating log files only pre-allocates a small number.
class test_bug019(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled,file_max=100K),statistics=(fast)'
    uri = "table:bug019"
    entries = 5000
    max_initial_entries = 50000
    max_prealloc = 1

    # Modify rows so we write log records. We're writing a lot more than a
    # single log file, so we know the underlying library will churn through
    # log files.
    def get_prealloc_used(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prealloc = stat_cursor[stat.conn.log_prealloc_used][2]
        stat_cursor.close()
        return prealloc

    def get_prealloc_stat(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prealloc = stat_cursor[stat.conn.log_prealloc_max][2]
        stat_cursor.close()
        return prealloc

    def populate(self, nentries, count):
        c = self.session.open_cursor(self.uri, None, None)
        min_entries = nentries // 10
        for i in range(0, nentries):
            # Make the values about 2000 bytes. When called with 5000 records
            # that's about 10MB of data, generating 100 log files used plus more for overhead.
            # Typically the huge traffic causes the preallocation statistic to
            # increase.  We'll quit when it does, as that's our goal here.
            # We wait for a minimum of 10% of the inserts before quitting because
            # we want to make sure this function consumes some log files. We
            # don't know when the internal log server thread will run and update
            # the statistic and we don't want to short-circuit without enough work.
            # For the initial populate, we'll insert up to 10x as many records,
            # so up to 1000 log files.
            #
            # Make the keys unique for each pass.
            key = str(count) + " I:" + str(i)
            c[key] = "abcde" * 400
            if i > min_entries and i % 50 == 0:
                prealloc = self.get_prealloc_stat()
                if prealloc > self.max_prealloc:
                    self.pr("Iter {}: Updating max_prealloc from {} to {} after {} inserts".
                            format(count, self.max_prealloc, prealloc, i))
                    self.max_prealloc = prealloc
                    break
        c.close()

    # Wait for a log file to be pre-allocated. Avoid timing problems, but
    # assert a file is created within 90 seconds.
    def prepfiles(self):
        for i in range(1,90):
            f = fnmatch.filter(os.listdir('.'), "*Prep*")
            if f:
                return
            time.sleep(1.0)
        self.fail('No pre-allocated files created after 90 seconds')

    # There was a bug where pre-allocated log files accumulated on
    # Windows systems due to an issue with the directory list code.
    def test_bug019(self):
        start_prealloc = self.get_prealloc_stat()
        self.max_prealloc = start_prealloc

        # Populate a new table to generate log traffic.  This typically
        # increase the max number of log files preallocated, as indicated by
        # the statistic.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.populate(self.max_initial_entries, 0)
        self.session.checkpoint()
        if self.max_prealloc <= start_prealloc:
            self.pr("FAILURE: max_prealloc " + str(self.max_prealloc))
            self.pr("FAILURE: start_prealloc " + str(start_prealloc))
        self.assertTrue(self.max_prealloc > start_prealloc)

        # Loop, making sure pre-allocation is working and the range is moving.
        self.pr("Check pre-allocation range is moving")
        # Wait for pre-allocation to start.
        self.prepfiles()
        used = self.get_prealloc_used()
        for i in range(1, 10):
            self.populate(self.entries, i)
            newused = self.get_prealloc_used()
            self.pr("Iteration " + str(i))
            self.pr("previous used " + str(used) + " now " + str(newused))

            # Make sure we're consuming pre-allocated files.
            if used >= newused:
                self.pr("FAILURE on Iteration " + str(i))
                self.pr("FAILURE: previous used " + str(used) + " now " + str(newused))
            self.assertTrue(used < newused)
            used = newused

            self.session.checkpoint()

        # Wait for a long time for pre-allocate to drop in an idle system
        # it should usually be fast, but on slow systems can take time.
        max_wait_time = 90
        for sleepcount in range(1,max_wait_time):
            new_prealloc = self.get_prealloc_stat()
            if new_prealloc < self.max_prealloc:
                break
            time.sleep(1.0)
        if sleepcount >= max_wait_time:
            self.pr("FAILURE: sleepcount " + str(sleepcount))
            self.pr("FAILURE: max_wait_time " + str(max_wait_time))
        self.assertTrue(sleepcount < max_wait_time)

if __name__ == '__main__':
    wttest.run()
