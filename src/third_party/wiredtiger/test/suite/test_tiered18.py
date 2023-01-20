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

import os, wiredtiger, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources, get_shared_conn_config
from wtscenario import make_scenarios

StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered18.py
#    Basic tiered storage shared API test.
class test_tiered18(wttest.WiredTigerTestCase, TieredConfigMixin):
    # Make scenarios for different cloud service providers
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered18', tiered_only=True, tiered_shared=True)
    scenarios = make_scenarios(storage_sources)

    uri_non_shared = "table:test_tiered18_non_shared"
    uri_shared = "table:test_tiered18_shared"
    uri_local = "table:test_tiered18_local"
    uri_fail = "table:test_tiered18_fail"

    retention = 3

    def conn_config(self):
        if self.ss_name == 'dir_store':
            os.mkdir(self.bucket)
            os.mkdir(self.bucket1)
        self.saved_conn = get_shared_conn_config(self) + 'local_retention=%d)'\
             % self.retention
        return self.saved_conn

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    # Check for a specific string as part of the uri's metadata.
    def check_metadata(self, uri, val_str):
        c = self.session.open_cursor('metadata:create')
        val = c[uri]
        c.close()
        self.assertTrue(val_str in val)

    # Test calling the create API with shared enabled.
    def test_tiered_shared(self):
        self.pr("create tiered")
        base_create = 'key_format=S,value_format=S'
        self.session.create(self.uri_non_shared, base_create)
        self.check_metadata(self.uri_non_shared, 'key_format=S')
        #self.session.drop(self.uri_non_shared)

        self.pr("create non tiered/local")
        conf = ',tiered_storage=(name=none)'
        self.session.create(self.uri_local, base_create + conf)
        self.check_metadata(self.uri_local, 'name=none')
        #self.session.drop(self.uri_local)

        self.pr("create tiered shared")
        conf = ',tiered_storage=(shared=true)'
        self.session.create(self.uri_shared, base_create + conf)
        #self.session.drop(self.uri_shared)

        self.reopen_conn(config = self.saved_conn + ',tiered_storage=(shared=false)')

        self.pr("create tiered shared with connection shared false")
        conf = ',tiered_storage=(shared=true)'
        err_msg = "/Invalid argument/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.session.create(self.uri_fail, "base_create + conf"), err_msg)

if __name__ == '__main__':
    wttest.run()
