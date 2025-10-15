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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered53.py
#    Check that we can create a checkpoint just to capture the stable timestamp update.
@disagg_test_class
class test_layered53(wttest.WiredTigerTestCase):
    disagg_storages = gen_disagg_storages('test_layered53', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = 'cache_size=10MB,statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    nitems = 10

    session_follow = None
    conn_follow = None

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

    def test_layered53(self):
        uri = "layered:test_layered53"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')
        self.create_follower()

        # Insert some data and checkpoint.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = str(i)
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")
        cursor.close()

        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")
        self.session.checkpoint()

        # Ensure that the checkpoint has the correct timestamp.
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(checkpoint_timestamp, 10)

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Advance the stable timestamp on the leader without dirtying anything, check that we indeed
        # created a checkpoint.
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)}")
        self.session.checkpoint()
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(checkpoint_timestamp, 20)

        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Check that the follower cannot do the same thing.
        cursor = self.session_follow.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session_follow.begin_transaction()
            cursor[str(i)] = str(i) + "b"
            self.session_follow.commit_transaction(f"commit_timestamp={self.timestamp_str(30)}")
        cursor.close()

        self.conn_follow.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")
        self.session_follow.checkpoint()
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(checkpoint_timestamp, 20)

        # Idempotence check: advancing the follower again should *not* change state.
        # It should simply log that the same checkpoint is being picked up again.
        # And two conditions implied here:
        # 1) The metadata LSN should not change.
        # 2) Error log should be raised.
        meta_lsn = self.disagg_get_complete_checkpoint_meta()
        with self.expectedStdoutPattern(".*Picking up the same checkpoint again.*"):
            self.disagg_advance_checkpoint(self.conn_follow)
        # Check that the metadata LSN did not change.
        current_meta_lsn = self.disagg_get_complete_checkpoint_meta()
        self.assertEqual(meta_lsn, current_meta_lsn)
