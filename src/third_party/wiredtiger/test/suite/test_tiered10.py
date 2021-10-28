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

# test_tiered10.py
#    Test tiered storage with simultaneous connections using different
# prefixes to the same bucket directory but different local databases.
class test_tiered10(wttest.WiredTigerTestCase):

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered10-000000000'
    fileuri_base = 'file:' + base
    obj1file = base + '1.wtobj'
    objuri = 'object:' + base + '1.wtobj'
    tiereduri = "tiered:test_tiered10"
    uri = "table:test_tiered10"

    auth_token = "test_token"
    bucket = "mybucket"
    bucket1 = "../" + bucket
    bucket2 = "../" + bucket
    conn1_dir = "first_dir"
    conn2_dir = "second_dir"
    extension_name = "local_store"
    prefix1 = "1_"
    prefix2 = "2_"
    retention = 1
    saved_conn = ''
    def conn_config(self):
        os.mkdir(self.bucket)
        os.mkdir(self.conn1_dir)
        os.mkdir(self.conn2_dir)
        # Use this to create the directories and set up for the others.
        dummy_conn = 'create,statistics=(all),' 
        self.saved_conn = \
          'create,statistics=(all),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=../%s,' % self.bucket + \
          'local_retention=%d,' % self.retention + \
          'name=%s),' % self.extension_name 
        return dummy_conn

    # Load the local store extension.
    def conn_extensions(self, extlist):
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.extension_name)

    def check(self, tc, base, n):
        for i in range(base, n):
            self.assertEqual(tc[str(i)], str(i))
        tc.set_key(str(n))
        self.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

    # Test calling the flush_tier API.
    def test_tiered(self):
        # Have two connections running in different directories, but sharing
        # the same bucket directory with different prefixes. Each database
        # creates an identically named table with different data. Each then
        # does a flush tier testing that both databases can coexist in the
        # same bucket without conflict.
        #
        # Then reopen the connections and make sure we can read data correctly.
        #
        # We open two connections manually so that they both have the same relative
        # pathnames. The standard connection is just a dummy for this test.
        ext = self.extensionsConfig()
        conn1_params = self.saved_conn + ext + ',tiered_storage=(bucket_prefix=%s)' % self.prefix1
        conn1 = self.wiredtiger_open(self.conn1_dir, conn1_params)
        session1 = conn1.open_session()
        conn2_params = self.saved_conn + ext + ',tiered_storage=(bucket_prefix=%s)' % self.prefix2
        conn2 = self.wiredtiger_open(self.conn2_dir, conn2_params)
        session2 = conn2.open_session()

        session1.create(self.uri, 'key_format=S,value_format=S,')
        session2.create(self.uri, 'key_format=S,value_format=S,')

        # Add first data. Checkpoint, flush and close the connection.
        c1 = session1.open_cursor(self.uri)
        c2 = session2.open_cursor(self.uri)
        c1["0"] = "0"
        c2["20"] = "20"
        self.check(c1, 0, 1)
        self.check(c2, 20, 1)
        c1.close()
        c2.close()
        session1.checkpoint()
        session1.flush_tier(None)
        session2.checkpoint()
        session2.flush_tier(None)
        conn1_obj1 = self.bucket + '/' + self.prefix1 + self.obj1file
        conn2_obj1 = self.bucket + '/' + self.prefix2 + self.obj1file
        self.assertTrue(os.path.exists(conn1_obj1))
        self.assertTrue(os.path.exists(conn2_obj1))
        conn1.close()
        conn2.close()

        # Remove the local copies of the objects before we reopen so that we force
        # the system to read from the bucket or bucket cache.
        local = self.conn1_dir + '/' + self.obj1file
        os.remove(local)
        local = self.conn2_dir + '/' + self.obj1file
        os.remove(local)

        conn1 = self.wiredtiger_open(self.conn1_dir, conn1_params)
        session1 = conn1.open_session()
        conn2 = self.wiredtiger_open(self.conn2_dir, conn2_params)
        session2 = conn2.open_session()

        c1 = session1.open_cursor(self.uri)
        c2 = session2.open_cursor(self.uri)
        self.check(c1, 0, 1)
        self.check(c2, 20, 1)
        c1.close()
        c2.close()

if __name__ == '__main__':
    wttest.run()
