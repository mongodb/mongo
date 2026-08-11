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

# test_layered_schema24.py
#    The shared metadata queue across a step-down. Pending creates, drops and publishes are schema
#    intents no checkpoint has covered yet, and their meaning does not depend on the node's role, so
#    they survive the transition and a later leader era completes them.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema24(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    table_config = 'key_format=S,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def uri(self, name):
        """Return a distinct layered table URI within this test."""
        return f'layered:{self.test_name}_{name}'

    def create_table_in_shared_metadata(self, uri, publish_epoch, stable_ts, commit_ts, rows=0):
        """Create, publish, and cover a table with a checkpoint so it reaches shared metadata."""
        self.session.create(uri, self.table_config)
        if rows:
            self.write_at(uri, {f'k{i}': 'covered' for i in range(rows)}, commit_ts)
        self.publish(uri, publish_epoch)
        self.set_stable_epoch(publish_epoch)
        self.leader_checkpoint(stable_ts)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))

    def create_with_pending_publish(self, uri, publish_epoch):
        """
        Create and publish a table above the last checkpoint's epoch, so its CREATE stays pending in
        the queue and the table is absent from shared metadata.
        """
        self.set_stable_epoch(10)
        self.session.create(uri, self.table_config)
        self.publish(uri, publish_epoch)
        self.leader_checkpoint(1)
        self.assert_table_state(self.conn, uri, True, False, False)

    def local_metadata_keys(self, conn, uri):
        """The local metadata keys naming the table or its constituents."""
        tablename = uri[len('layered:'):]
        session = conn.open_session('')
        cursor = session.open_cursor('metadata:')
        keys = [k for k, _ in cursor if tablename in k]
        cursor.close()
        session.close()
        return keys

    def assert_follower_absent(self, uri):
        """A fresh follower picking up the latest checkpoint must not see the table."""
        conn_follow, session_follow = self.open_follower()
        self.assertFalse(self.uri_in_local_metadata(conn_follow, uri))
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, uri))
        self.close_follower(conn_follow, session_follow)

    def assert_follower_reads(self, uri, expected, conn=None):
        """A follower picking up the latest checkpoint reads the expected contents."""
        if conn is None:
            conn_follow, session_follow = self.open_follower()
        else:
            conn_follow, session_follow = conn, conn.open_session('')
        cursor = session_follow.open_cursor(uri)
        self.assertEqual({k: v for k, v in cursor}, expected)
        cursor.close()
        if conn is None:
            self.close_follower(conn_follow, session_follow)
        else:
            session_follow.close()

    def test_pending_create_survives_repeated_failback(self):
        """
        A pending publish survives repeated fail-backs with its constituent reused each time, and
        the reused constituent then serves writes that a covering checkpoint publishes.
        """
        uri = self.uri('pending')
        rows = {'k1': 'pending', 'k2': 'pending'}
        self.create_with_pending_publish(uri, 20)

        for _ in range(3):
            self.step_down()
            self.assertTrue(self.stable_constituent_exists(self.conn, uri))
            self.step_up()
            self.assert_table_state(self.conn, uri, True, False, False)

        self.write_at(uri, rows, 2)
        self.set_stable_epoch(20)
        self.leader_checkpoint(3)
        self.assert_table_state(self.conn, uri, True, True, True)
        self.assert_follower_reads(uri, rows)

    def test_pending_create_survives_noncovering_pickup(self):
        """
        A pending publish survives a step-down and a pickup of a checkpoint that does not cover its
        epoch: the pickup must not prune the CREATE that explains the local constituent.
        """
        uri = self.uri('noncovering')
        rows = {'k1': 'noncovering', 'k2': 'noncovering'}
        self.create_with_pending_publish(uri, 30)

        # A second node picks up the checkpoint at epoch 10 and does not see the table.
        conn_follow, session_follow = self.open_follower()
        session_follow.close()
        self.assertFalse(self.uri_in_local_metadata(conn_follow, uri))

        # Swap roles: this node steps down with the CREATE at epoch 30 still pending.
        self.step_down()
        self.step_up(conn_follow)

        # The new leader checkpoints at epoch 20, which does not cover the pending CREATE.
        self.set_stable_epoch(20, conn_follow)
        session_follow = conn_follow.open_session('')
        self.leader_checkpoint(2, conn_follow, session_follow)
        session_follow.close()

        self.disagg_advance_checkpoint(self.conn, conn_follow)
        self.assert_table_state(self.conn, uri, True, False, False)

        # Fail back and checkpoint at a covering epoch: the table is published with its rows.
        self.step_down(conn_follow)
        self.step_up()
        self.write_at(uri, rows, 3)
        self.set_stable_epoch(30)
        self.leader_checkpoint(4)
        self.assert_table_state(self.conn, uri, True, True, True)

        # The other node picks up the covering checkpoint and reads the rows.
        self.disagg_advance_checkpoint(conn_follow)
        self.assert_follower_reads(uri, rows, conn_follow)
        self.close_follower(conn_follow)

    def test_two_phase_drop_survives_failback(self):
        """
        A drop published above the last checkpoint epoch leaves a pending REMOVE that survives
        repeated fail-backs, and the next covering checkpoint applies it.
        """
        uri = self.uri('drop')
        self.set_stable_epoch(10)
        self.create_table_in_shared_metadata(uri, 20, 1, 5)

        # Drop and publish the drop at an epoch no checkpoint has covered yet.
        self.session.drop(uri)
        self.publish(uri, 30)
        self.assertEqual(self.local_metadata_keys(self.conn, uri), [])

        for _ in range(3):
            self.step_down()
            self.step_up()

        self.set_stable_epoch(30)
        self.leader_checkpoint(2)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))
        self.assert_follower_absent(uri)

    def test_create_drop_pair_nets_out(self):
        """
        A create and drop published at the same pending epoch survive a step-down as a queued pair,
        and the next covering checkpoint nets them out without ever publishing the table.
        """
        uri = self.uri('pair')
        self.set_stable_epoch(10)
        self.session.create(uri, self.table_config)
        self.publish(uri, 20)

        # A checkpoint below the publish epoch leaves the CREATE pending.
        self.leader_checkpoint(1)
        self.assert_table_state(self.conn, uri, True, False, False)

        self.session.drop(uri)
        self.publish(uri, 20)
        self.assertEqual(self.local_metadata_keys(self.conn, uri), [])

        self.step_down()
        self.step_up()
        self.assertFalse(self.uri_in_local_metadata(self.conn, uri))

        self.set_stable_epoch(20)
        self.leader_checkpoint(2)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))
        self.assert_follower_absent(uri)
