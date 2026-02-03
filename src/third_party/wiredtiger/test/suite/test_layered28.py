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

import re, os, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_layered28.py
#    Test to ensure that dropping layered tables works and subsequent sweep doesn't crash
@disagg_test_class
class test_layered28(wttest.WiredTigerTestCase):
    uri_base = "test_layered28"
    conn_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),disaggregated=(role="leader"),' \
                + 'file_manager=(close_scan_interval=1)'

    table_types = [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('table-prefix', dict(prefix='table:', table_config=',block_manager=disagg,type=layered')),
    ]

    disagg_storages = gen_disagg_storages('test_key_provider_disagg02', disagg_only = True)
    scenarios = make_scenarios(table_types, disagg_storages)
    def check_metadata_entry(self):
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key("file:" + self.uri_base + ".wt_stable")
        self.assertEqual(meta_cursor.search(), wiredtiger.WT_NOTFOUND)
        meta_cursor.set_key("file:" + self.uri_base + ".wt_ingest")
        self.assertEqual(meta_cursor.search(), wiredtiger.WT_NOTFOUND)
        meta_cursor.set_key("layered:" + self.uri_base)
        self.assertEqual(meta_cursor.search(), wiredtiger.WT_NOTFOUND)

        if (self.prefix == "table:"):
            meta_cursor.set_key("table:" + self.uri_base)
            self.assertEqual(meta_cursor.search(), wiredtiger.WT_NOTFOUND)
            meta_cursor.set_key("colgroup:" + self.uri_base)
            self.assertEqual(meta_cursor.search(), wiredtiger.WT_NOTFOUND)
        meta_cursor.close()

    # Ensure that the shared metadata has removed the table.
    def check_shared_metadata(self, expect_exists):
        expected_ret = 0 if expect_exists else wiredtiger.WT_NOTFOUND
        cursor = self.session.open_cursor('file:WiredTigerShared.wt_stable', None, None)

        cursor.set_key("file:" + self.uri_base + ".wt_stable")
        self.assertEqual(cursor.search(), expected_ret)
        cursor.set_key("layered:" + self.uri_base)
        self.assertEqual(cursor.search(), expected_ret)

        if (self.prefix == "table:"):
            cursor.set_key("colgroup:" + self.uri_base)
            self.assertEqual(cursor.search(), expected_ret)
            cursor.set_key("table:" + self.uri_base)
            self.assertEqual(cursor.search(), expected_ret)
        cursor.close()

    def validate_drop(self):
        # Validate that all metadata entries are removed.
        self.check_metadata_entry()

        # Validate that we can't open a cursor on the dropped table.
        uri = self.prefix + self.uri_base
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda:self.session.open_cursor(uri, None, None))

    # Test simple create and drop on leader mode.
    def test_create_drop(self):
        base_create = 'key_format=S,value_format=S' + self.table_config
        uri = self.prefix + self.uri_base

        self.pr("create layered tree")
        self.session.create(uri, base_create)

        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[str(i)] = str(i)
        cursor.close()

        self.session.checkpoint()
        self.session.drop(uri, "")
        self.validate_drop()

        # Persist schema drop operation to shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_exists=False)

    # Test create and drop with a subsequent checkpoint and enough time for sweep to come through
    def test_create_drop_checkpoint(self):
        base_create = 'key_format=S,value_format=S' + self.table_config
        uri = self.prefix + self.uri_base

        # Use a session so it can be closed which releases the reference to the dhandle and
        # allows the sweep thread to close out the handle
        custom_session = self.conn.open_session()
        self.pr("create layered tree")
        custom_session.create(uri, base_create)

        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[str(i)] = str(i)
        cursor.close()

        custom_session.checkpoint()
        custom_session.drop(uri, "")
        custom_session.close()
        self.validate_drop()

        # Persist schema drop operation to shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_exists=False)

    # Test create and drop on follower mode.
    def test_create_drop_follower(self):
        base_create = 'key_format=S,value_format=S' + self.table_config
        uri = self.prefix + self.uri_base

        self.pr("create layered tree")
        self.session.create(uri, base_create)

        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[str(i)] = str(i)
        cursor.close()

        self.session.checkpoint()
        # Get the checkpoint metadata before closing
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Configure as follower with checkpoint pickup (not using backup)
        follower_config = 'disaggregated=(role="follower",' + \
                         f'checkpoint_meta="{checkpoint_meta}")'

        # Switch to follower mode.
        self.reopen_conn(config=follower_config)
        self.session.drop(uri, "")
        self.validate_drop()

        # Persist schema drop operation to shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_exists=True)
