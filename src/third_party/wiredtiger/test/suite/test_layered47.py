#!/usr/bin/env python3
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

import errno, os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Regression test for WT-15158
# Before the fix, during the initialization of an ingest table, `prune_timestamp` was initialized to the last checkpoint timestamp.
# During the subsequent checkpoint operation, it should be updated to the most recent checkpoint in use.
# This test is designed to initialize the `prune_timestamp` to `ckpt2` while `ckpt1` is still in use,
# in order to create a conflict by updating the timestamp to an older one (to `ckpt1`) during the checkpoint operation.

@disagg_test_class
class test_layered47(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered47', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Keep it low to avoid splitting pages
    nitems = 10
    timestamp = 2

    conn_base_config = 'disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'

    session_follow = None
    conn_follow = None

    uris = ['layered:test_layered47.a', 'layered:test_layered47.b']

    def leader_put_data(self, uri, value_prefix = '', low = 1, high = nitems):
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(low, high):
            self.session.begin_transaction()
            cursor[str(i)] = value_prefix + str(i)
            self.timestamp += 1
            ts_cfg = "commit_timestamp=" + self.timestamp_str(self.timestamp)
            self.session.commit_transaction(ts_cfg)
        cursor.close()

    def checkpoint(self):
        self.timestamp += 1
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.timestamp))
        self.session.checkpoint()

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

    def test_layered47(self):
        # Setup
        self.session.create(self.uris[0], self.table_cfg)
        self.session.create(self.uris[1], self.table_cfg)
        self.create_follower()

        # Create and pick-up checkpoint #1
        self.leader_put_data(self.uris[1])
        self.leader_put_data(self.uris[0])
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Open a cursor on uris[0] to pin ckpt as in use
        cursor = self.session_follow.open_cursor(self.uris[0], None, None)
        self.session_follow.begin_transaction()
        cursor.set_key(str(1))
        cursor.search()
        self.session_follow.commit_transaction()

        # Create and pick-up checkpoint #2
        self.leader_put_data(self.uris[0], 'aaa')
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Initialize prune_timestamp (should be set to the ckpt 2, but ckpt 1 is still in use)
        cursor2 = self.session_follow.open_cursor(self.uris[1], None, None)

        # Create and pick-up checkpoint #3 to initiate checkpoint pickup for `uris[1]`
        self.leader_put_data(self.uris[0], 'bbb')
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        cursor.close()
        self.session_follow.close()
        self.conn_follow.close()
