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

from wtscenario import make_scenarios
import wiredtiger, wttest
from wiredtiger import stat
import os, random, string, time

# test_io_capacity01.py
#   Max waiting period for background fsync. If the written threshold is not met in this time,
#   A background fsync is done.
class test_io_capacity01(wttest.WiredTigerTestCase):
    uri = 'table:test_io_capacity01'
    collection_cfg = 'key_format=q,value_format=S'
    fsync_time = 1
    # Set the io_capacity config
    open_config = 'create,statistics=(all),io_capacity=(' + 'fsync_maximum_wait_period=' + str(fsync_time) + ',total=1M)'

    def generate_random_string(self, length):
        characters = string.digits
        random_string = ''.join(random.choice(characters) for _ in range(length))
        return random_string

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    # The number of written bytes exceeded the threshold.
    def test_io_capacity_written_threshold(self):
        # Close the initial connection. We will be opening new connections for this test.
        self.close_conn()

        # For a total of 1024 data writes of 1024 bytes each, the total number of bytes written is exactly 1M.
        # The reconcile operation will add some additional data to ensure that the "total=1M" is triggered
        insert_count = 1024
        value_size = 1024
        random_string = self.generate_random_string(value_size)

        conn = self.wiredtiger_open(self.home, self.open_config)
        self.session = conn.open_session()
        self.session.create(self.uri, self.collection_cfg)
        cursor = self.session.open_cursor(self.uri)
        for i in range(insert_count):
            cursor.set_key(i)
            cursor.set_value(random_string)
            cursor.insert()

        # Take a checkpoint to ensure that the data is written to the disk
        self.session.checkpoint('force=true')

        if os.name == 'nt':
           self.skipTest('skipped on Windows as the capacity server will not run without support for background fsync')
        else:
            # Background fsync statistics
            self.assertGreater(self.get_stat(stat.conn.fsync_all_fh_total), 0)

    # The number of written bytes not exceeded the threshold, but the running period is exceeded
    def test_io_capacity_fsync_background_period(self):
        # Close the initial connection. We will be opening new connections for this test.
        self.close_conn()

        # Only insert 1 data, the write bytes is well below 1M
        # If the written conditions are not met fsync_maximum_wait_period, force background fsync
        insert_count = 1
        value_size = 1024
        retry_times = 0
        random_string = self.generate_random_string(value_size)
        conn = self.wiredtiger_open(self.home, self.open_config)

        self.session = conn.open_session()

        self.session.create(self.uri, self.collection_cfg)
        cursor = self.session.open_cursor(self.uri)
        for i in range(insert_count):
            cursor.set_key(i)
            cursor.set_value(random_string)
            cursor.insert()

        # Take a checkpoint to ensure that the data is written to the disk
        self.session.checkpoint('force=true')

        if os.name == "nt":
           self.skipTest('skipped on Windows as the capacity server will not run without support for background fsync')

        # Background fsync statistics
        while (self.get_stat(stat.conn.fsync_all_fh_total) == 0):
            retry_times += 1
            time.sleep(0.1)
            # Config fsync_maximum_wait_period=1
            # Considering the scenario of high server load, we ensure that the waiting time is greater than 60 second
            if retry_times > 601:
                raise Exception("Timed out waiting for fsync_all_fh_total statistic")

if __name__ == '__main__':
    wttest.run()
