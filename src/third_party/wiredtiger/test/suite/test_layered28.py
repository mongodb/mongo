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
from helper_disagg import disagg_test_class
from wiredtiger import stat

# test_layered28.py
#    Test to ensure that dropping layered tables works and subsequent sweep doesn't crash
@disagg_test_class
class test_layered28(wttest.WiredTigerTestCase):
    uri_base = "test_layered28"
    conn_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),disaggregated=(role="leader"),' \
                + 'disaggregated=(page_log=palm),file_manager=(close_scan_interval=1)'

    uri = "layered:" + uri_base

    # Load the directory store extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    # Test simple create and drop
    def test_create_drop(self):
        base_create = 'key_format=S,value_format=S,type=layered'

        self.pr("create layered tree")
        self.session.create(self.uri, base_create)

        self.session.drop(self.uri, "")

    # Test create and drop with a subsequent checkpoint and enough time for sweep to come through
    def test_create_drop_checkpoint(self):
        base_create = 'key_format=S,value_format=S'

        # Use a session so it can be closed which releases the reference to the dhandle and
        # allows the sweep thread to close out the handle
        custom_session = self.conn.open_session()
        self.pr("create layered tree")
        custom_session.create(self.uri, base_create)

        custom_session.checkpoint()
        custom_session.drop(self.uri, "")
        custom_session.close()

        time.sleep(2)


