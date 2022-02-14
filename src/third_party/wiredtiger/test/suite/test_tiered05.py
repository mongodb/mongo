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
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered05.py
#    Basic tiered storage API test error for tiered manager and flush_tier.
class test_tiered05(wttest.WiredTigerTestCase):
    uri = "table:test_tiered05"

    auth_token = "test_token"
    bucket = "my_bucket"
    bucket_prefix = "my_prefix"
    extension_name = "local_store"
    wait = 2

    def conn_extensions(self, extlist):
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.extension_name)

    def conn_config(self):
        os.mkdir(self.bucket)
        return \
          'tiered_manager=(wait=%d),' % self.wait + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.bucket_prefix + \
          'name=%s,' % self.extension_name + \
          'object_target_size=20M)'

    # Test calling the flush_tier API with a tiered manager. Should get an error.
    def test_tiered(self):
        self.session.create(self.uri, 'key_format=S')
        # Allow time for the thread to start up.
        time.sleep(self.wait)
        msg = "/storage manager thread is configured/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.flush_tier(None), 0), msg)

if __name__ == '__main__':
    wttest.run()
