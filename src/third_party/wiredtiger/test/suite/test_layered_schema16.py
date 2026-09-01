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

# Test disaggregated table publication across create, drop, and recreate sequences.
#
# A table's publish status is decided from its latest create/remove entry in the metadata queue.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema16(wttest.WiredTigerTestCase, suite_subprocess, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def write_unstable_row(self):
        """Write a row at timestamp 10, above the stable timestamp used by these tests."""
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()

    def subprocess_checkpoint_below_published_recreated_drop_panics(self):
        """Subprocess body: a checkpoint below a published drop with a recreate pending panics."""
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        # Checkpoint the original table and its row so later checkpoints can refer to this
        # generation.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()
        self.set_stable_epoch(2)
        self.leader_checkpoint(2)

        # Drop the table and recreate it, publishing both above the next checkpoint's epoch.
        self.session.drop(self.uri)
        self.publish(self.uri, 4)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)

        # The checkpoint at epoch 3 refers to the dropped generation while the recreate is
        # queued behind the drop: an API violation.
        self.set_stable_epoch(3)
        self.leader_checkpoint(3)  # Expected to panic.

    def test_checkpoint_below_published_recreated_drop_panics(self):
        """
        A checkpoint whose schema epoch is below a table's published drop cannot be satisfied once
        the table has been recreated: the epoch refers to a generation whose data the drop
        destroyed while a new generation exists. WiredTiger panics rather than emit a checkpoint
        with missing durable rows. A checkpoint below a published drop with no recreate is legal.
        """
        # Set timestamps so the fixture can close the parent connection cleanly; the real test
        # runs in a subprocess so that the panic/abort does not kill the test runner.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.run_panic_subprocess('checkpoint_below_published_recreated_drop_panics',
            'checkpoint schema epoch refers to a dropped and recreated table')

    def test_recreate_redropped_before_checkpoint_is_legal(self):
        """
        A recreated generation dropped again before the checkpoint runs leaves nothing for the
        checkpoint to resolve: a checkpoint below the original published drop carries the first
        generation forward, exactly as a plain drop does.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        # Checkpoint the original table and its row so later checkpoints can refer to this
        # generation.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()
        self.set_stable_epoch(2)
        self.leader_checkpoint(2)

        # Drop, re-create, and drop again, all published above the next checkpoint's epoch.
        self.session.drop(self.uri)
        self.publish(self.uri, 4)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)
        self.session.drop(self.uri)
        self.publish(self.uri, 6)

        # The checkpoint at epoch 3 still refers to the first generation: legal, no panic.
        self.set_stable_epoch(3)
        self.leader_checkpoint(3)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, self.uri))

        # Once the epoch covers all three operations, the table leaves shared metadata.
        self.set_stable_epoch(6)
        self.leader_checkpoint(6)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri))

    def test_create_drop_recreate_above_checkpoint_epoch(self):
        """
        A table created, dropped and recreated entirely above the checkpoint's schema epoch never
        existed at that epoch: all three entries defer without a violation, and the recreated
        table appears once the epoch covers it.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        # Create, drop, re-create and publish all three, all above the next checkpoint's epoch.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)
        self.session.drop(self.uri)
        self.publish(self.uri, 6)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 7)

        # The checkpoint at epoch 3 predates the table entirely: no violation.
        self.set_stable_epoch(3)
        self.leader_checkpoint(3)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri))

        # Once the epoch covers all three operations they apply in order: the create/drop pair
        # cancels and the recreated table appears.
        self.set_stable_epoch(7)
        self.leader_checkpoint(7)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, self.uri))

    def test_recreate_above_stable_epoch_not_resurrected(self):
        """
        A table dropped at or below the stable schema epoch and recreated above it must not survive
        recovery. The recreate stays unpublished at that epoch and is absent after recovery.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        self.session.drop(self.uri)
        self.publish(self.uri, 3)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 9)
        self.write_unstable_row()

        # Checkpoint with the epoch below the recreate; the recreate stays unpublished.
        self.set_stable_epoch(5)
        self.leader_checkpoint(5)
        self.restart_without_local_files(step_up=True)

        # A recovered leader re-establishes its stable timestamp before it can checkpoint again.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(5) +
            ',oldest_timestamp=' + self.timestamp_str(1))

        # After recovery, the drop is the latest visible operation; the table must be absent.
        # Assert neither constituent survived, so a partial resurrection cannot slip through.
        self.assertFalse(self.uri_stable_exists(self.conn, self.uri))
        self.assertFalse(self.uri_in_local_metadata(self.conn, self.uri))

    def test_unpublished_table_holds_unstable_data(self):
        """
        An unpublished table may legitimately hold unstable data. The checkpoint skips it without
        raising a violation.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        # Epoch 9 is above the stable schema epoch, so the table stays unpublished.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 9)
        self.write_unstable_row()

        self.leader_checkpoint(5)

        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri))

    def test_create_then_drop_not_published(self):
        """
        A table created and dropped at or below the stable schema epoch is absent from shared
        metadata.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        self.session.drop(self.uri)
        self.publish(self.uri, 3)

        self.set_stable_epoch(5)
        self.leader_checkpoint(5)

        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri))

    def test_recreate_publishes_latest_create(self):
        """
        A table created, dropped, and recreated publishes on the latest create once the stable
        schema epoch reaches it. The stale earlier create does not interfere.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        self.session.drop(self.uri)
        self.publish(self.uri, 3)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 9)
        self.write_unstable_row()

        # The epoch and stable timestamp now cover the recreate, so it publishes.
        self.set_stable_epoch(9)
        self.leader_checkpoint(10)

        self.assertTrue(self.uri_in_shared_metadata(self.conn, self.uri))
