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
from wiredtiger import stat
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered12.py
#    Test tiered storage with tiered flush finish timing delay.
class test_tiered12(wttest.WiredTigerTestCase):

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered12-000000000'
    obj1file = base + '1.wtobj'
    uri = "table:test_tiered12"

    auth_token = "test_token"
    bucket = "mybucket"
    cache = "cache-mybucket"
    extension_name = "local_store"
    prefix1 = "1_"
    retention = 1
    saved_conn = ''
    def conn_config(self):
        os.mkdir(self.bucket)
        self.saved_conn = \
          'statistics=(all),timing_stress_for_test=(tiered_flush_finish),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.prefix1 + \
          'local_retention=%d,' % self.retention + \
          'name=%s)' % self.extension_name 
        return self.saved_conn

    # Load the local store extension.
    def conn_extensions(self, extlist):
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.extension_name)

    def check(self, tc, n):
        for i in range(0, n):
            self.assertEqual(tc[str(i)], str(i))
        tc.set_key(str(n))
        self.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

    def test_tiered(self):
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
        self.check(c, 1)
        c.close()
        self.session.checkpoint()

        bucket_obj = os.path.join(self.bucket, self.prefix1 + self.obj1file)
        cache_obj = os.path.join(self.cache, self.prefix1 + self.obj1file)
        self.session.flush_tier(None)
        # Immediately after flush_tier finishes the cached object should not yet exist
        # but the bucket object does exist.
        self.assertFalse(os.path.exists(cache_obj))
        self.assertTrue(os.path.exists(bucket_obj))
        # Sleep more than the one second stress timing amount and give the thread time to run.
        time.sleep(2)
        # After sleeping, the internal thread should have created the cached object.
        self.assertTrue(os.path.exists(cache_obj))

if __name__ == '__main__':
    wttest.run()
