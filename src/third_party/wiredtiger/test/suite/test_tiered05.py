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

from helper_tiered import generate_s3_prefix, get_auth_token, get_bucket1_name
import os, time, wiredtiger, wttest
from wtscenario import make_scenarios
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered05.py
#    Basic tiered storage API test error for tiered manager and flush_tier.
class test_tiered05(wttest.WiredTigerTestCase):
    storage_sources = [
        ('local', dict(auth_token = get_auth_token('local_store'),
            bucket = get_bucket1_name('local_store'),
            bucket_prefix = "pfx_",
            ss_name = 'local_store')),
        ('s3', dict(auth_token = get_auth_token('s3_store'),
            bucket = get_bucket1_name('s3_store'),
            bucket_prefix = generate_s3_prefix(),
            ss_name = 's3_store')),
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    uri = "table:test_tiered05"
    wait = 2

    def conn_extensions(self, extlist):
        config = ''
        # S3 store is built as an optional loadable extension, not all test environments build S3.
        if self.ss_name == 's3_store':
            #config = '=(config=\"(verbose=1)\")'
            extlist.skip_if_missing = True
        #if self.ss_name == 'local_store':
            #config = '=(config=\"(verbose=1,delay_ms=200,force_delay=3)\")'
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.ss_name + config)

    def conn_config(self):
        if self.ss_name == 'local_store' and not os.path.exists(self.bucket):
            os.mkdir(self.bucket)
        return \
          'tiered_manager=(wait=%d),' % self.wait + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.bucket_prefix + \
          'name=%s,' % self.ss_name + \
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
