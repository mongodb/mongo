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

# test_tiered09.py
#    Test tiered storage with sequential connections with different prefixes.
class test_tiered09(wttest.WiredTigerTestCase):
    storage_sources = [
        ('local', dict(auth_token = get_auth_token('local_store'),
            bucket = get_bucket1_name('local_store'),
            prefix1 = '1_',
            prefix2 = '2_',
            prefix3 = '3_',
            ss_name = 'local_store')),
        ('s3', dict(auth_token = get_auth_token('s3_store'),
            bucket = get_bucket1_name('s3_store'),
            prefix1 = generate_s3_prefix(),
            prefix2 = generate_s3_prefix(),
            prefix3 = generate_s3_prefix(),
            ss_name = 's3_store')),
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered09-000000000'
    base2 = 'test_second09-000000000'
    obj1file = base + '1.wtobj'
    obj1second = base2 + '1.wtobj'
    obj2file = base + '2.wtobj'
    uri = "table:test_tiered09"
    uri2 = "table:test_second09"

    retention = 1
    saved_conn = ''
    def conn_config(self):
        if self.ss_name == 'local_store' and not os.path.exists(self.bucket):
            os.mkdir(self.bucket)
        self.saved_conn = \
          'debug_mode=(flush_checkpoint=true),' + \
          'statistics=(all),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.prefix1 + \
          'local_retention=%d,' % self.retention + \
          'name=%s)' % self.ss_name 
        return self.saved_conn

    # Load the storage store extension.
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

    def check(self, tc, n):
        for i in range(0, n):
            self.assertEqual(tc[str(i)], str(i))
        tc.set_key(str(n))
        self.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

    # Test calling the flush_tier API.
    def test_tiered(self):
        # Create a table. Add some data. Checkpoint and flush tier.
        # Close the connection. Then we want to reopen the connection
        # with a different bucket prefix and repeat. Then reopen the
        # connection with the original prefix. Then reopen and verify
        # we can read all the data.
        #
        # Verify the files are as we expect also. We expect:
        # 1_<tablename>-00000001.wtobj
        # 2_<tablename>-00000002.wtobj
        # 1_<tablename>-00000003.wtobj
        # but we can read and access all data in all objects.
        self.session.create(self.uri, 'key_format=S,value_format=S,')
        # Add first data. Checkpoint, flush and close the connection.
        c = self.session.open_cursor(self.uri)
        c["0"] = "0"
        self.check(c, 1)
        c.close()
        self.session.checkpoint()
        self.session.flush_tier(None)
        self.close_conn()

        # For local store, check if the path exists.
        if self.ss_name == 'local_store':
            self.assertTrue(os.path.exists(self.obj1file))
            self.assertTrue(os.path.exists(self.obj2file))
            bucket_obj = os.path.join(self.bucket, self.prefix1 + self.obj1file)
            self.assertTrue(os.path.exists(bucket_obj))

        # Since we've closed and reopened the connection we lost the work units
        # to drop the local objects. Clean them up now to make sure we can open
        # the correct object in the bucket.
        localobj = './' + self.obj1file
        os.remove(localobj)

        # Reopen the connection with a different prefix this time.
        conn_params = self.saved_conn + ',tiered_storage=(bucket_prefix=%s)' % self.prefix2
        self.conn = self.wiredtiger_open('.', conn_params)
        self.session = self.conn.open_session()
        # Add a second table created while the second prefix is used for the connection.
        self.session.create(self.uri2, 'key_format=S,value_format=S,')
        # Add first data. Checkpoint, flush and close the connection.
        c = self.session.open_cursor(self.uri2)
        c["0"] = "0"
        self.check(c, 1)
        c.close()
        # Add more data to original table.
        # Checkpoint, flush and close the connection.
        c = self.session.open_cursor(self.uri)
        c["1"] = "1"
        self.check(c, 2)
        c.close()
        self.session.checkpoint()
        self.session.flush_tier(None)
        self.close_conn()

        # For local store, Check each table was created with the correct prefix.
        if self.ss_name == 'local_store':
            bucket_obj = os.path.join(self.bucket, self.prefix2 + self.obj1second)
            self.assertTrue(os.path.exists(bucket_obj))
            bucket_obj = os.path.join(self.bucket, self.prefix1 + self.obj2file)
            self.assertTrue(os.path.exists(bucket_obj))

        # Since we've closed and reopened the connection we lost the work units
        # to drop the local objects. Clean them up now to make sure we can open
        # the correct object in the bucket.
        localobj = './' + self.obj2file
        os.remove(localobj)
        localobj = './' + self.obj1second

        # Reopen with the other prefix and check all data. Even though we're using the
        # other prefix, we should find all the data in the object with the original
        # prefix.
        conn_params = self.saved_conn + ',tiered_storage=(bucket_prefix=%s)' % self.prefix3
        self.conn = self.wiredtiger_open('.', conn_params)
        self.session = self.conn.open_session()
        c = self.session.open_cursor(self.uri)
        self.check(c, 2)
        c.close()
        c = self.session.open_cursor(self.uri2)
        self.check(c, 1)
        c.close()

if __name__ == '__main__':
    wttest.run()
