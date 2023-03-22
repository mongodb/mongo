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

import os, time, wiredtiger, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources, get_conn_config, get_check
from wtscenario import make_scenarios
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered12.py
#    Basic tiered storage API test error for tiered manager and flush_tier.
class test_tiered12(wttest.WiredTigerTestCase, TieredConfigMixin):
    # Make scenarios for different cloud service providers
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered12', tiered_only=True)
    scenarios = make_scenarios(storage_sources)

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered12-000000000'
    obj1file = base + '1.wtobj'
    uri = "table:test_tiered12"

    retention = 1
    saved_conn = ''
    def conn_config(self):
        self.saved_conn = get_conn_config(self) + 'local_retention=%d),' \
            % self.retention + 'timing_stress_for_test=(tiered_flush_finish)'
        return self.saved_conn

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def check(self, tc, base, n):
        get_check(self, tc, base, n)

    def test_tiered(self):
        # Default cache location is cache-<bucket-name>
        cache = "cache-" + self.bucket
        # The bucket format for the S3 store is the name and the region separataed by a semi-colon.
        # Strip off the region to get the cache folder.
        if self.ss_name == 's3_store':
            cache = cache[:cache.find(';')]  

        # Create a table. Add some data. Checkpoint and flush tier.
        # We have configured the timing stress for tiered caching which delays
        # the internal thread calling flush_finish for 1 second.
        # So after flush tier completes, check that the cached object does not
        # exist. Then sleep and check that it does exist.
        #
        # The idea is to make sure flush_tier is not waiting for unnecessary work
        # to be done, but returns as soon as the copying to shared storage completes.
        self.session.create(self.uri, 'key_format=S,value_format=S,')

        # Add data. Checkpoint and flush.
        c = self.session.open_cursor(self.uri)
        c["0"] = "0"
        self.check(c, 0, 1)
        c.close()
        # Use force to make sure the new object is created. Otherwise there is no
        # existing checkpoint yet and the flush will think there is no work to do.
        self.session.checkpoint('flush_tier=(enabled,force=true)')

        # On directory store, the bucket object should exist.
        if self.ss_name == 'dir_store':
            bucket_obj = os.path.join(self.bucket, self.bucket_prefix + self.obj1file)
            self.assertTrue(os.path.exists(bucket_obj))

        # Sleep more than the one second stress timing amount and give the thread time to run.
        time.sleep(2)
        # After sleeping, the internal thread should have created the cached object.
        if self.has_cache:
            cache_obj = os.path.join(cache, self.bucket_prefix + self.obj1file)
            self.assertTrue(os.path.exists(cache_obj))

if __name__ == '__main__':
    wttest.run()
