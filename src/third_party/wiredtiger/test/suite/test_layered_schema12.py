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

# Test the interaction between unpublished schema changes and concurrent
# transactions, and the lifecycle of tables created and dropped without ever
# reaching a checkpoint.
#
# An unpublished table (created but not yet published with a schema epoch)
# behaves like a newly created table that has not been included in any
# checkpoint: it must never appear in the shared metadata, and it must not
# obstruct unrelated writes or checkpoints. A table that is created and then
# dropped before either operation is published must leave no durable trace.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema12(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    uri2 = f'layered:{test_name}_b'
    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def write(self, uri, key, value, commit_ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def assertOnFollower(self, present):
        """
        Pick up the latest checkpoint on a fresh follower and assert which of the
        expected tables are present in both the local and shared metadata.
        present maps uri -> bool.
        """
        conn, _ = self.open_follower()
        try:
            for uri, expected in present.items():
                if expected:
                    self.assertTrue(self.uri_in_local_metadata(conn, uri))
                    self.assertTrue(self.uri_in_shared_metadata(conn, uri))
                else:
                    self.assertFalse(self.uri_in_local_metadata(conn, uri))
                    self.assertFalse(self.uri_in_shared_metadata(conn, uri))
        finally:
            conn.close('debug=(skip_checkpoint=true)')

    #
    # Transaction interaction tests
    #

    def test_unrelated_writes_succeed_with_unpublished_table(self):
        """
        A table that is created but not yet published has a pending schema change.
        Writes to other, published tables must continue to succeed while that schema
        change remains pending, and the pending table must not become durable.
        """
        self.set_stable_epoch(10)

        # A published table that unrelated writes target.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)

        # A table created but never published: its schema change is still pending.
        self.session.create(self.uri2, self.table_config)

        # Unrelated writes to the published table must succeed while uri2 is unpublished.
        self.set_stable_epoch(20)
        for i in range(10):
            self.write(self.uri, i, 'value', 30 + i)

        # A checkpoint covering the unrelated writes must also succeed with the
        # schema change still pending.
        self.leader_checkpoint(40)

        # The published table is durable in both local and shared metadata; the
        # unpublished table appears in neither.
        self.assertOnFollower({self.uri: True, self.uri2: False})

    def test_checkpoint_succeeds_with_unpublished_table(self):
        """
        A checkpoint taken while a table is unpublished must succeed, provided the
        unpublished table has no writes at or below the stable timestamp. The
        published table's stable writes are checkpointed as normal.
        """
        self.set_stable_epoch(10)

        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        self.set_stable_epoch(20)

        # Create an unpublished table alongside the published one.
        self.session.create(self.uri2, self.table_config)

        # Stable writes to the published table, none to the unpublished table.
        self.write(self.uri, 1, 'value', 50)

        # The checkpoint must succeed even though uri2 remains unpublished.
        self.leader_checkpoint(50)

        # The published table is durable; the unpublished table is in neither the
        # shared nor the local metadata.
        self.assertOnFollower({self.uri: True, self.uri2: False})

    def test_unpublished_table_not_in_durable_metadata(self):
        """
        A table left unpublished when a checkpoint is taken must not leak into the
        durable (shared) metadata. Followers picking up the checkpoint must not see
        the unpublished table, while the published table is visible.
        """
        self.set_stable_epoch(10)

        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)

        # uri2 is created but deliberately never published.
        self.session.create(self.uri2, self.table_config)

        self.set_stable_epoch(20)
        self.leader_checkpoint(1)

        # The published table is durable; the unpublished table is not.
        self.assertOnFollower({self.uri: True, self.uri2: False})

    #
    # Table lifecycle tests
    #

    def test_create_drop_unpublished_not_durable(self):
        """
        Create and drop a table without ever publishing either operation and without
        an intervening checkpoint. The transient table must leave no durable trace:
        it appears in neither the shared nor the local metadata of a follower after a
        subsequent checkpoint.
        """
        self.set_stable_epoch(10)

        # An unrelated published table to give the checkpoint something to persist.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        self.set_stable_epoch(20)

        # Create then drop uri2, both unpublished and before any checkpoint.
        self.session.create(self.uri2, self.table_config)
        self.session.drop(self.uri2)

        self.leader_checkpoint(1)

        # uri2 never becomes durable; uri does.
        self.assertOnFollower({self.uri: True, self.uri2: False})

    def test_create_publish_drop_publish_reaped_before_checkpoint(self):
        """
        Create, publish, drop, and publish the drop of a table, all before any
        checkpoint incorporates the create. When the stable epoch finally advances
        past both operations and a checkpoint is taken, the create and the drop
        cancel out: the table must never appear in a checkpoint.
        """
        self.set_stable_epoch(10)

        # An unrelated table that must remain durable throughout.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)

        # Full create/publish/drop/publish lifecycle for uri2 while the stable epoch
        # is still behind, so neither operation is applied to a checkpoint yet.
        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 20)
        self.session.drop(self.uri2)
        self.publish(self.uri2, 30)

        # Advance past both epochs and checkpoint: the CREATE and REMOVE for uri2 are
        # both eligible and cancel out.
        self.set_stable_epoch(30)
        self.leader_checkpoint(1)

        self.assertOnFollower({self.uri: True, self.uri2: False})

    def test_create_drop_unpublished_not_resurrected_on_restart(self):
        """
        A table created and dropped without publishing, and without a checkpoint that
        includes it, must not be resurrected when the node restarts from the shared
        checkpoint.
        """
        self.set_stable_epoch(10)

        # Establish a durable table and checkpoint so there is something to restart from.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        self.set_stable_epoch(20)
        self.write(self.uri, 1, 'value', 50)
        self.leader_checkpoint(50)

        # Create and drop uri2 without publishing or checkpointing it.
        self.session.create(self.uri2, self.table_config)
        self.session.drop(self.uri2)

        # Restart, discarding local files and picking up the shared checkpoint.
        self.restart_without_local_files(step_up=True)

        # Re-establish the stable timestamp so the shutdown checkpoint can proceed.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(50) +
            ',oldest_timestamp=' + self.timestamp_str(1))

        # The durable table survives; the transient table is not resurrected.
        self.assertTrue(self.uri_in_local_metadata(self.conn, self.uri))
        self.assertFalse(self.uri_in_local_metadata(self.conn, self.uri2))
        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri2))
