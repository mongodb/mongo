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
# [TEST_TAGS]
# connection_api:reconfigure
# [END_TAGS]

import time
import wiredtiger, wttest
from wtdataset import SimpleDataSet

# test_reconfig01.py
#    Smoke-test the connection reconfiguration operations.
class test_reconfig01(wttest.WiredTigerTestCase):

    def test_reconfig_shared_cache(self):
        self.conn.reconfigure("shared_cache=(name=pool,size=300M)")

    def test_reconfig_eviction(self):
        # Increase the max number of running threads (default 8).
        self.conn.reconfigure("eviction=(threads_max=10)")
        # Increase the min number of running threads (default 1).
        self.conn.reconfigure("eviction=(threads_min=5)")
        # Decrease the max number of running threads.
        self.conn.reconfigure("eviction=(threads_max=7)")
        # Decrease the min number of running threads.
        self.conn.reconfigure("eviction=(threads_min=2)")
        # Set min and max the same.
        self.conn.reconfigure("eviction=(threads_min=6,threads_max=6)")
        # Set target and trigger with an absolute value.
        self.conn.reconfigure("eviction_target=50M,eviction_trigger=100M")
        # Set dirty target and trigger with an absolute value
        self.conn.reconfigure("eviction_dirty_target=20M,"
                              "eviction_dirty_trigger=40M")
        # Set eviction checkpoint target with an absolute value
        self.conn.reconfigure("eviction_checkpoint_target=50M")

    def test_reconfig_lsm_manager(self):
        # We create and populate a tiny LSM so that we can start off with
        # the LSM threads running and change the numbers of threads.
        # Take all the defaults.
        uri = "lsm:test_reconfig"
        nrecs = 10
        SimpleDataSet(self, uri, nrecs).populate()
        # Sleep to make sure all threads are started.
        time.sleep(2)
        # Now that an LSM tree exists, reconfigure LSM manager threads.
        # We start with the default, which is 4.  Configure more threads.
        self.conn.reconfigure("lsm_manager=(worker_thread_max=10)")
        # Generate some work
        nrecs = 20
        SimpleDataSet(self, uri, nrecs).populate()
        # Now reconfigure fewer threads.
        self.conn.reconfigure("lsm_manager=(worker_thread_max=3)")

    def test_reconfig_statistics(self):
        self.conn.reconfigure("statistics=(all)")
        self.conn.reconfigure("statistics=(fast)")
        self.conn.reconfigure("statistics=(none)")

    def test_reconfig_capacity(self):
        self.conn.reconfigure("io_capacity=(total=80M)")
        self.conn.reconfigure("io_capacity=(total=100M)")
        msg = '/below minimum/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("io_capacity=(total=16K)"), msg)

    def test_reconfig_checkpoints(self):
        self.conn.reconfigure("checkpoint=(wait=0)")
        self.conn.reconfigure("checkpoint=(wait=5)")
        self.conn.reconfigure("checkpoint=(log_size=0)")
        self.conn.reconfigure("checkpoint=(log_size=1M)")

    # Statistics logging: reconfigure the things we can reconfigure.
    def test_reconfig_statistics_log_ok(self):
        self.conn.reconfigure("statistics=[all],statistics_log=(wait=0)")
        self.conn.reconfigure("statistics_log=(wait=0)")
        self.conn.reconfigure("statistics_log=(wait=2,json=true)")
        self.conn.reconfigure("statistics_log=(wait=0)")
        self.conn.reconfigure("statistics_log=(wait=2,on_close=true)")
        self.conn.reconfigure("statistics_log=(wait=0)")
        self.conn.reconfigure("statistics_log=(wait=2,sources=[lsm:])")
        self.conn.reconfigure("statistics_log=(wait=0)")
        self.conn.reconfigure("statistics_log=(wait=2,timestamp=\"t%b %d\")")
        self.conn.reconfigure("statistics_log=(wait=0)")

    # Statistics logging: reconfigure the things we can't reconfigure.
    def test_reconfig_statistics_log_fail(self):
        msg = '/unknown configuration key/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(path=foo)"), msg)

    def test_file_manager(self):
        self.conn.reconfigure("file_manager=(close_scan_interval=3)")
        self.conn.reconfigure("file_manager=(close_idle_time=4)")
        self.conn.reconfigure(
            "file_manager=(close_idle_time=4,close_scan_interval=100)")

if __name__ == '__main__':
    wttest.run()
