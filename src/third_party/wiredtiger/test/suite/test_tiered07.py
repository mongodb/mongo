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
from wtscenario import make_scenarios
import os, wiredtiger, wttest
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered07.py
#    Basic tiered storage API for schema operations.
class test_tiered07(wttest.WiredTigerTestCase):
    storage_sources = [
        ('local', dict(auth_token = get_auth_token('local_store'),
            bucket = get_bucket1_name('local_store'),
            bucket_prefix = "pfx_",
            ss_name = 'local_store')),
        # FIXME-WT-8897 Disabled as S3 directory listing is interpreting a directory to end in a '/',
        # whereas the code in the tiered storage doesn't expect that. Enable when fixed.
        #('s3', dict(auth_token = get_auth_token('s3_store'),
        #    bucket = get_bucket1_name('s3_store'),
        #    bucket_prefix = generate_s3_prefix(),
        #    ss_name = 's3_store'))
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    uri = "table:abc"
    uri2 = "table:ab"
    uri3 = "table:abcd"
    localuri = "table:local"
    newuri = "table:tier_new"

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
        #  'verbose=(tiered),' + \

        return \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.bucket_prefix + \
          'name=%s,' % self.ss_name + \
          'object_target_size=20M)'

    def check(self, tc, n):
        for i in range(0, n):
            self.assertEqual(tc[str(i)], str(i))
        tc.set_key(str(n))
        self.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

    # Test calling schema APIs with a tiered table.
    def test_tiered(self):
        # Create a new tiered table.
        self.pr('create table')
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.pr('create table 2')
        self.session.create(self.uri2, 'key_format=S,value_format=S')
        self.pr('create table 3')
        self.session.create(self.uri3, 'key_format=S,value_format=S')
        self.pr('create table local')
        self.session.create(self.localuri, 'key_format=S,value_format=S,tiered_storage=(name=none)')

        # Rename is not supported for tiered tables.
        msg = "/is not supported/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.rename(self.uri, self.newuri, None), 0), msg)

        # Add some data and flush tier.
        self.pr('add one item to all tables')
        c = self.session.open_cursor(self.uri)
        c["0"] = "0"
        self.check(c, 1)
        c.close()
        c = self.session.open_cursor(self.uri2)
        c["0"] = "0"
        self.check(c, 1)
        c.close()
        c = self.session.open_cursor(self.uri3)
        c["0"] = "0"
        self.check(c, 1)
        c.close()
        c = self.session.open_cursor(self.localuri)
        c["0"] = "0"
        c.close()
        self.session.checkpoint()
        self.pr('After data, call flush_tier')
        self.session.flush_tier(None)

        # Drop table.
        self.pr('call drop')
        self.session.drop(self.localuri)
        self.session.drop(self.uri)

        # Create new table with same name. This should error.
        msg = "/already exists/"
        self.pr('check cannot create with same name')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.create(self.uri, 'key_format=S'), 0), msg)

        # Make sure there was no problem with overlapping table names.
        self.pr('check original similarly named tables')
        c = self.session.open_cursor(self.uri2)
        self.check(c, 1)
        c.close()
        c = self.session.open_cursor(self.uri3)
        self.check(c, 1)
        c.close()

        # Create new table with new name.
        self.pr('create new table')
        self.session.create(self.newuri, 'key_format=S')

if __name__ == '__main__':
    wttest.run()
