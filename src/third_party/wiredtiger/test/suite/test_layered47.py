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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered47.py
#    Test pruning of ingest tables on the follower during checkpoint pick-ups.
@disagg_test_class
class test_layered47(wttest.WiredTigerTestCase):
    disagg_storages = gen_disagg_storages('test_layered47', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Keep it low to avoid splitting pages
    nitems = 10
    timestamp = 2

    conn_base_config = ''
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'

    session_follow = None
    conn_follow = None

    def setup(self, uris):
        '''
        Setup: create tables, put some content to them and pick them up on the follower
        Checkpoint order for both tables is 1 after this step
        '''
        for uri in uris:
            self.session.create(uri, self.table_cfg)
            self.leader_put_data(uri)

        self.create_follower()
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

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

    def follower_open_close_dummy_cursor(self, uri):
        cursor = self.session_follow.open_cursor(uri, None, None)
        cursor.close()

    # Regression test for WT-15158
    # Before the fix, during the initialization of an ingest table, `prune_timestamp` was set to the last checkpoint timestamp.
    # During a subsequent checkpoint operation, it should be updated to the most recent checkpoint currently in use.
    # This test initializes the `prune_timestamp` to `ckpt2` while `ckpt1` is still in use,
    # creating a conflict by attempting to update the timestamp to an older checkpoint (`ckpt1`) during the checkpoint operation.
    def test_prune_timestamp_initialization(self):
        uris = ['layered:test_layered47.a', 'layered:test_layered47.b']
        self.setup(uris)

        # Open a cursor on uris[0] to pin ckpt as in use
        cursor = self.session_follow.open_cursor(uris[0], None, None)
        self.session_follow.begin_transaction()
        cursor.set_key(str(1))
        cursor.search()
        self.session_follow.commit_transaction()

        # Create and pick-up checkpoint #2
        self.leader_put_data(uris[0], 'aaa')
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Initialize prune_timestamp (should be set to the ckpt 2, but ckpt 1 is still in use)
        cursor2 = self.session_follow.open_cursor(uris[1], None, None)

        # Create and pick-up checkpoint #3 to initiate checkpoint pickup for `uris[1]`
        self.leader_put_data(uris[0], 'bbb')
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        cursor2.close()
        cursor.close()
        self.session_follow.close()
        self.conn_follow.close()

    # Regression test for WT-15192
    # The root cause of the issue reproduced here is that the logic for selecting a prune
    # timestamp was based on the metadata checkpoint order. However, this order is table-local,
    # and different tables could have a different order for the same checkpoint, so that logic
    # could easily be broken.
    def test_checkpoint_order_mismatch(self):
        uris = ['layered:test_layered47.a', 'layered:test_layered47.b']
        self.setup(uris)

        # Open ingest tables on the follower to make them participate in pruning during pick-ups
        self.follower_open_close_dummy_cursor(uris[0])
        self.follower_open_close_dummy_cursor(uris[1])

        # Create more checkpoints for the first table only
        # Assuming that checkpoint order for metadata and uris[0] after that is 11
        for i in range(0, 10):
            self.leader_put_data(uris[0], str(i))
            self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Put more content and create new checkpoint for uris[1]
        # Checkpoint order for it would be 2 which is different from metadata and uris[0] checkpoints
        self.leader_put_data(uris[1])
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        self.session_follow.close()
        self.conn_follow.close()

    # Test setting prune TS when previous checkpoints weren't needed it and cursor is open
    def test_first_gc_with_cursor_on_previous_checkpoint(self):
        uri = 'layered:test_layered47.a'
        self.setup([uri])

        # Create 3 checkpoints with a content on the leader and pick them up on the follower
        # No cursors on the follower were opened so far, so the didn't participate in ingest GC
        for i in range(0, 3):
            self.leader_put_data(uri, str(i))
            self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Cursor is pointing to the last checkpoint that exists at this point (ckpt #4)
        cursor = self.session_follow.open_cursor(uri)
        cursor.next()

        # Add ckpt #5
        self.leader_put_data(uri)
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        self.session_follow.close()
        self.conn_follow.close()
