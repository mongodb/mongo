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
#
# test_layered_schema30.py
#    Two nodes that disagree about schema epochs. A node runs in epoch mode only if the
#    application sets the stable schema epoch on it, and runs epochless otherwise, so a rolling
#    upgrade or downgrade pairs an epoch-mode node with an epochless one. Cover both role
#    directions and restarts across the change.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema30(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    table_config = 'key_format=S,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # The epoch the epoch-mode leader starts out at.
    seed_epoch = 10

    def uri(self, name):
        """A distinct layered table URI."""
        return f'layered:{self.test_name}_{name}'

    def create_table(self, name, rows=None, commit_ts=None, session=None):
        """Create a table on a node, and fill it if the test gave it rows."""
        session = session or self.session
        uri = self.uri(name)
        session.create(uri, self.table_config)
        if rows:
            cursor = session.open_cursor(uri)
            session.begin_transaction()
            for k, v in rows.items():
                cursor[k] = v
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            cursor.close()
        return uri

    def create_published(self, name, rows=None, commit_ts=None, epoch=None):
        """Create a table and publish it. Publishing above the stable epoch leaves it gated."""
        uri = self.create_table(name, rows, commit_ts)
        self.publish(uri, epoch)
        return uri

    def restart_leader(self, epoch=None):
        """Restart the leader. Without an epoch it comes up epochless."""
        self.restart_without_local_files(config=self.conn_config_follower, step_up=True)
        if epoch is not None:
            self.set_stable_epoch(epoch)

    def restart_follower(self, conn, session=None, epoch=None):
        """Restart a follower. Startup deletes its local files, so it comes back from shared
        storage alone, and epochless unless given an epoch."""
        self.close_follower(conn, session)
        if epoch is None:
            return self.open_follower()
        return self.open_follower_epoch(epoch)

    def assert_dropped(self, conn, uri):
        """The node no longer advertises the table. A pickup leaves the local metadata row."""
        self.assertFalse(self.uri_in_shared_metadata(conn, uri))

    def assert_carried_epoch(self, conn, carried):
        """The epoch the node's last checkpoint carried."""
        self.assertEqual(
            int(conn.query_timestamp('get=last_disaggregated_schema_epoch'), 16), carried)

    def assert_epochs(self, conn, live, carried):
        """The node's live epoch, zero when it is epochless, and the carried one."""
        self.assertEqual(
            int(conn.query_timestamp('get=stable_disaggregated_schema_epoch'), 16), live)
        self.assert_carried_epoch(conn, carried)

    def seed_epoch_world(self, rows):
        """Publish a table on the epoch-mode leader and checkpoint it at the seed epoch."""
        self.set_stable_epoch(1)
        uri = self.create_published('seeded', rows, commit_ts=10, epoch=self.seed_epoch)
        self.set_stable_epoch(self.seed_epoch)
        self.leader_checkpoint(20)
        self.assert_carried_epoch(self.conn, self.seed_epoch)
        return uri

    def test_epoch_leader_epochless_follower_reads(self):
        """An epochless follower reads an epoch-mode leader's published tables and nothing more."""
        published = self.seed_epoch_world({'k1': 'published'})

        # A second table published above the stable epoch, so no checkpoint covers it yet.
        gated = self.create_published('gated', {'k1': 'gated'}, commit_ts=45, epoch=20)
        self.leader_checkpoint(40)
        self.assert_table_state(self.conn, published, True, True, True)
        self.assert_table_state(self.conn, gated, True, False, False)

        # The follower never sets an epoch, but the pickup still hands it the leader's.
        conn_follow, session_follow = self.open_follower()
        self.assert_epochs(conn_follow, 0, self.seed_epoch)

        self.assertEqual(self.read_kvs_at(published, 40, session_follow), {'k1': 'published'})

        # The gate holds for an epochless node, because it lives in the checkpoint.
        self.assertFalse(self.uri_in_local_metadata(conn_follow, gated))
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, gated))

        # Once the leader's epoch covers the table, the follower picks it up like any other.
        self.set_stable_epoch(20)
        self.leader_checkpoint(50)
        self.disagg_advance_checkpoint(conn_follow)
        self.assert_carried_epoch(conn_follow, 20)
        self.assertTrue(self.uri_stable_exists(conn_follow, gated))
        self.assertEqual(self.read_kvs_at(gated, 50, session_follow), {'k1': 'gated'})

        self.close_follower(conn_follow, session_follow)

    def test_epochless_follower_steps_up_and_carries_epoch(self):
        """An epochless node takes over from an epoch-mode one without losing the epoch."""
        seeded = self.seed_epoch_world({'k1': 'seed'})

        # An uncovered create, which the leader still holds in its queue when it hands over.
        gated = self.create_published('gated', epoch=20)
        self.leader_checkpoint(30)

        conn_follow, session_follow = self.open_follower()
        self.step_down()
        self.step_up(conn_follow)
        self.assert_epochs(conn_follow, 0, self.seed_epoch)

        # The new leader rebuilds from what the checkpoint gave it, which excludes the gated table.
        self.assertTrue(self.uri_stable_exists(conn_follow, seeded))
        self.assertFalse(self.uri_stable_exists(conn_follow, gated))

        # Its own create needs no publish, and the checkpoint must keep the cluster's epoch.
        during_epochless = self.create_table(
            'during_epochless', {'k1': 'epochless'}, commit_ts=40, session=session_follow)
        self.leader_checkpoint(50, conn_follow, session_follow)
        self.assert_table_state(conn_follow, during_epochless, True, True, True)
        self.assert_carried_epoch(conn_follow, self.seed_epoch)

        # A table the epoch-mode era created drops without a publish too.
        session_follow.drop(seeded)
        self.leader_checkpoint(60, conn_follow, session_follow)
        self.assertFalse(self.uri_stable_exists(conn_follow, seeded))
        self.assert_dropped(conn_follow, seeded)
        self.assert_carried_epoch(conn_follow, self.seed_epoch)

        self.close_follower(conn_follow, session_follow)

    def test_epoch_leader_returns_after_epochless_era(self):
        """An epoch-mode node reclaims the lead after an epochless one held it."""
        seeded = self.seed_epoch_world({'k1': 'seed'})

        gated = self.create_published('gated', {'k1': 'gated'}, commit_ts=35, epoch=20)
        self.leader_checkpoint(30)

        # The epochless node leads for a while, and adds a table of its own.
        conn_follow, session_follow = self.open_follower()
        self.step_down()
        self.step_up(conn_follow)
        during_epochless = self.create_table(
            'during_epochless', {'k1': 'epochless'}, commit_ts=40, session=session_follow)
        self.leader_checkpoint(50, conn_follow, session_follow)

        # The epoch-mode node's live epoch keeps the pickup from pruning the create it still holds.
        self.disagg_advance_checkpoint(self.conn, conn_follow)
        self.assert_carried_epoch(self.conn, self.seed_epoch)
        self.assertTrue(self.uri_stable_exists(self.conn, during_epochless))

        # Taking the lead back replays the queue, so the uncovered create is still there.
        self.step_down(conn_follow)
        self.step_up()
        self.assertTrue(self.uri_stable_exists(self.conn, gated))

        # Raising the epoch over it publishes it at last.
        self.set_stable_epoch(30)
        self.leader_checkpoint(60)
        self.assert_table_state(self.conn, gated, True, True, True)
        self.assert_table_state(self.conn, during_epochless, True, True, True)
        self.assert_carried_epoch(self.conn, 30)
        self.assertEqual(self.read_kvs_at(seeded, 60), {'k1': 'seed'})
        self.assertEqual(self.read_kvs_at(during_epochless, 60), {'k1': 'epochless'})

        self.close_follower(conn_follow, session_follow)

    def test_epochless_leader_epoch_follower_steps_up(self):
        """An epoch-mode follower keeps its queue under an epochless leader and leads with it."""
        self.seed_epoch_world(rows={'k1': 'seed'})
        self.restart_leader()
        self.assert_epochs(self.conn, 0, self.seed_epoch)

        during_epochless = self.create_table('during_epochless', {'k1': 'epochless'}, commit_ts=30)
        self.leader_checkpoint(40)

        # The follower enters epoch mode at the epoch the leader is carrying.
        conn_follow, session_follow = self.open_follower_epoch(self.seed_epoch)
        self.assert_epochs(conn_follow, self.seed_epoch, self.seed_epoch)
        self.assertEqual(self.read_kvs_at(during_epochless, 40, session_follow), {'k1': 'epochless'})

        # A gated create on the follower stays in its queue.
        gated = self.create_table('gated_on_follower', session=session_follow)
        self.publish(gated, 20, session_follow)
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, gated))

        # More checkpoints from the epochless leader move neither the epoch nor the queue.
        self.leader_checkpoint(50)
        self.disagg_advance_checkpoint(conn_follow)
        self.assert_carried_epoch(conn_follow, self.seed_epoch)

        # The follower leads, and its step-up rebuilds the queued table.
        self.step_down()
        self.step_up(conn_follow)
        self.assertTrue(self.uri_stable_exists(conn_follow, gated))

        # The table is still gated, and publishing it takes the epoch it was published at.
        self.leader_checkpoint(60, conn_follow, session_follow)
        self.assert_table_state(conn_follow, gated, True, False, False)
        self.assert_carried_epoch(conn_follow, self.seed_epoch)

        self.set_stable_epoch(20, conn_follow)
        self.leader_checkpoint(70, conn_follow, session_follow)
        self.assert_table_state(conn_follow, gated, True, True, True)
        self.assert_carried_epoch(conn_follow, 20)

        # The epochless node consumes the result.
        self.disagg_advance_checkpoint(self.conn, conn_follow)
        self.assertTrue(self.uri_stable_exists(self.conn, gated))
        self.assert_carried_epoch(self.conn, 20)

        self.close_follower(conn_follow, session_follow)

    def test_epoch_leader_drop_survives_epochless_step_up(self):
        """An epoch-mode leader's drop holds when an epochless node picks it up and takes the lead."""
        self.set_stable_epoch(1)
        kept = self.create_published('kept', {'k1': 'kept'}, commit_ts=10, epoch=self.seed_epoch)
        dropped = self.create_published(
            'dropped', {'k1': 'dropped'}, commit_ts=10, epoch=self.seed_epoch)
        self.set_stable_epoch(self.seed_epoch)
        self.leader_checkpoint(20)

        # The epochless node picks both tables up.
        conn_follow, session_follow = self.open_follower()
        self.assertTrue(self.uri_stable_exists(conn_follow, dropped))

        # An epoch-mode drop is published like any other schema operation, so the checkpoint covering
        # its epoch is the one that removes the table from shared metadata.
        self.session.drop(dropped)
        self.publish(dropped, 20)
        self.set_stable_epoch(20)
        self.leader_checkpoint(30)
        self.assert_dropped(self.conn, dropped)

        self.disagg_advance_checkpoint(conn_follow)
        self.assert_dropped(conn_follow, dropped)

        # The pickup leaves the dropped table's local metadata row behind, and an epochless step-up
        # rebuilds from that same local metadata, so this is where the table would come back.
        self.step_down()
        self.step_up(conn_follow)
        self.leader_checkpoint(40, conn_follow, session_follow)
        self.assert_dropped(conn_follow, dropped)
        self.assert_table_state(conn_follow, kept, True, True, True)
        self.assertEqual(self.read_kvs_at(kept, 40, session_follow), {'k1': 'kept'})

        self.close_follower(conn_follow, session_follow)

    def test_epochless_leader_schema_operations_reach_epoch_follower(self):
        """An epochless leader's creates and drops reach an epoch-mode follower through checkpoints."""
        self.seed_epoch_world({'k1': 'seed'})
        self.restart_leader()

        conn_follow, session_follow = self.open_follower_epoch(self.seed_epoch)

        # Two creates with no publish, carried by the checkpoint alone.
        kept = self.create_table('kept', {'k1': 'kept'}, commit_ts=30)
        dropped = self.create_table('dropped', {'k1': 'dropped'}, commit_ts=30)
        self.leader_checkpoint(40)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertTrue(self.uri_stable_exists(conn_follow, kept))
        self.assertTrue(self.uri_stable_exists(conn_follow, dropped))
        self.assertEqual(self.read_kvs_at(dropped, 40, session_follow), {'k1': 'dropped'})

        # The epoch-mode follower must let go of the table even though no epoch ever covered it.
        self.session.drop(dropped)
        self.leader_checkpoint(50)
        self.disagg_advance_checkpoint(conn_follow)
        self.assert_dropped(conn_follow, dropped)
        self.assertTrue(self.uri_stable_exists(conn_follow, kept))
        self.assertEqual(self.read_kvs_at(kept, 50, session_follow), {'k1': 'kept'})

        # The pickup left the local metadata row behind, so the step-up must not advertise it.
        self.step_down()
        self.step_up(conn_follow)
        self.leader_checkpoint(60, conn_follow, session_follow)
        self.assert_dropped(conn_follow, dropped)
        self.assert_table_state(conn_follow, kept, True, True, True)

        # Nor does handing the lead back resurrect it.
        self.disagg_advance_checkpoint(self.conn, conn_follow)
        self.step_down(conn_follow)
        self.step_up()
        self.leader_checkpoint(70)
        self.assert_dropped(self.conn, dropped)
        self.assert_table_state(self.conn, kept, True, True, True)
        self.assertEqual(self.read_kvs_at(kept, 70), {'k1': 'kept'})

        self.close_follower(conn_follow, session_follow)

    def test_toggle_epoch_mode_across_restarts_of_both_nodes(self):
        """Both nodes leave epoch mode and return to it, and no era's data is lost."""
        era_on1 = self.seed_epoch_world({'k1': 'era1'})
        conn_follow, session_follow = self.open_follower_epoch(self.seed_epoch)
        self.assertEqual(self.read_kvs_at(era_on1, 20, session_follow), {'k1': 'era1'})

        # Both nodes come back without setting an epoch.
        conn_follow, session_follow = self.restart_follower(conn_follow, session_follow)
        self.restart_leader()
        self.assert_epochs(self.conn, 0, self.seed_epoch)
        self.assert_epochs(conn_follow, 0, self.seed_epoch)

        # The epochless era adds a table of its own, published by the checkpoint alone.
        era_epochless = self.create_table('era_epochless', {'k1': 'era2'}, commit_ts=30)
        self.leader_checkpoint(40)
        self.disagg_advance_checkpoint(conn_follow)
        self.assert_table_state(self.conn, era_epochless, True, True, True)
        self.assertEqual(self.read_kvs_at(era_on1, 40, session_follow), {'k1': 'era1'})
        self.assertEqual(self.read_kvs_at(era_epochless, 40, session_follow), {'k1': 'era2'})
        self.assert_carried_epoch(conn_follow, self.seed_epoch)

        # Both nodes return to epoch mode, at or above the epoch the epochless era carried.
        conn_follow, session_follow = self.restart_follower(conn_follow, session_follow, epoch=20)
        self.restart_leader(epoch=20)
        self.assert_epochs(self.conn, 20, self.seed_epoch)

        # Gating really resumed: the new table waits for an epoch that covers it.
        era_on2 = self.create_published('era_on2', {'k1': 'era3'}, commit_ts=65, epoch=30)
        self.leader_checkpoint(60)
        self.assert_table_state(self.conn, era_on2, True, False, False)

        self.set_stable_epoch(30)
        self.leader_checkpoint(70)
        self.assert_table_state(self.conn, era_on2, True, True, True)
        self.assert_carried_epoch(self.conn, 30)

        self.disagg_advance_checkpoint(conn_follow)
        for uri, rows in ((era_on1, {'k1': 'era1'}),
                          (era_epochless, {'k1': 'era2'}),
                          (era_on2, {'k1': 'era3'})):
            self.assert_table_state(self.conn, uri, True, True, True)
            self.assertEqual(self.read_kvs_at(uri, 70), rows)
            self.assertEqual(self.read_kvs_at(uri, 70, session_follow), rows)

        self.close_follower(conn_follow, session_follow)
