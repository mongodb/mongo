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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_checkpoint19.py
#    Checkpoints around a role change on a connection that picked up a checkpoint as a follower:
#    a checkpoint taken immediately after step-up must continue the picked-up lineage, and a
#    forced checkpoint must advance the global checkpoint on a leader but not on a follower.
@disagg_test_class
class test_layered_checkpoint19(wttest.WiredTigerTestCase):
    test_name = __qualname__
    num_items = 100

    conn_base_config = 'statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f'layered:{test_name}'

    def open_follower(self):
        return self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config +
            'disaggregated=(role="follower")')

    def insert_data(self, session, value_prefix, ts):
        session.begin_transaction()
        cursor = session.open_cursor(self.uri)
        for i in range(self.num_items):
            cursor[str(i)] = value_prefix + str(i)
        cursor.close()
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')

    def check_data(self, session, value_prefix):
        cursor = session.open_cursor(self.uri)
        for i in range(self.num_items):
            self.assertEqual(cursor[str(i)], value_prefix + str(i))
        cursor.close()

    def complete_checkpoint_lsn(self, conn):
        return self.disagg_get_complete_checkpoint_ext(conn)[0]

    def test_checkpoint_immediately_after_step_up(self):
        # Node 1 checkpoints its data; node 2 picks it up, then writes more as a follower.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_data(self.session, 'v1-', 10)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        conn2 = self.open_follower()
        session2 = conn2.open_session('')
        self.disagg_advance_checkpoint_and_wait(conn2)
        self.insert_data(session2, 'v2-', 20)

        # Node 2 steps up and checkpoints immediately, draining its ingest.
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn2.reconfigure('disaggregated=(role="leader")')
        conn2.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        session2.checkpoint()

        # Node 1 picks up node 2's checkpoint and must see node 2's data.
        self.disagg_advance_checkpoint_and_wait(self.conn, conn2)
        self.check_data(self.session, 'v2-')

    def test_force_checkpoint_leader_and_follower(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_data(self.session, 'v1-', 10)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        conn2 = self.open_follower()
        session2 = conn2.open_session('')
        self.disagg_advance_checkpoint_and_wait(conn2)

        # A forced checkpoint on the follower checkpoints no shared table.
        before = self.complete_checkpoint_lsn(conn2)
        session2.checkpoint('force=true')
        # The assert below is making sure that the forced checkpoint
        # on the follower did not advance the checkpoint LSN.
        self.assertEqual(self.complete_checkpoint_lsn(conn2), before)

        # A forced checkpoint on the leader advances the global checkpoint.
        before = self.complete_checkpoint_lsn(self.conn)
        self.session.checkpoint('force=true')
        self.assertGreater(self.complete_checkpoint_lsn(self.conn), before)
