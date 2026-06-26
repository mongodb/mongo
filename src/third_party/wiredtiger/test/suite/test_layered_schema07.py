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

# Test WT_SESSION::publish for disaggregated storage.
#
# Schema operations (create, drop) on a leader do not get included in the next checkpoint
# until they are published with a schema epoch.

import os, time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
from wiredtiger import stat

# Test WT_SESSION::publish for disaggregated storage.
@disagg_test_class
class test_layered_schema07(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def set_stable_epoch(self, epoch):
        """
        Set stable_disaggregated_schema_epoch.
        """
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(epoch))

    def leader_checkpoint(self, stable_ts):
        """
        Set the oldest and stable timestamps, and then take a timestamped checkpoint.
        """
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_ts) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def open_follower(self):
        """
        Open a new connection with the follower configuration.
        """
        conn_follower = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        return conn_follower

    def table_exists_on_follower(self, uri):
        """
        Check if the table exists on the follower, by opening a follower and picking up the latest
        checkpoint. If the table is not visible, opening a cursor on it will fail with an error.
        """
        conn_follower = self.open_follower()
        self.disagg_advance_checkpoint(conn_follower)
        session_follower = conn_follower.open_session('')
        exists = True
        try:
            c = session_follower.open_cursor(uri)
            c.close()
        except wiredtiger.WiredTigerError:
            exists = False
        session_follower.close()
        conn_follower.close()
        return exists

    def publish(self, uri, epoch, session=None):
        """
        Publish a schema change with the given epoch. If session is None, use the main test session.
        """
        if session is None:
            session = self.session
        session.publish(uri, 'disaggregated=(schema_epoch=' + self.timestamp_str(epoch) + ')')


    #
    # Functional tests
    #

    def test_create_deferred_until_publish(self):
        """
        A table created after a stable epoch is set is invisible to followers
        until published; one checkpoint after publish makes it visible.
        """
        self.set_stable_epoch(5)
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Checkpoint 1: The table is not yet visible.
        self.leader_checkpoint(1)
        self.assertFalse(self.table_exists_on_follower(self.uri),
            'table should be invisible before publish')

        # Publish with epoch 10, and set the stable epoch to 10, which makes the table visible.
        self.publish(self.uri, 10)
        self.set_stable_epoch(10)

        # Checkpoint 2: The table is now visible.
        self.leader_checkpoint(2)
        self.assertTrue(self.table_exists_on_follower(self.uri),
            'table should be visible after publish and checkpoint')

    def test_drop_deferred_until_publish(self):
        """
        A drop of a published table is deferred until the drop is itself
        published; one checkpoint after drop-publish removes the table.
        """
        # Create and publish the table so that it gets included in the checkpoint.
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.publish(self.uri, 10)
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)
        self.assertTrue(self.table_exists_on_follower(self.uri),
            'table should be visible after create publish')

        # Drop the table, but do not publish yet.
        self.session.drop(self.uri)
        self.set_stable_epoch(20)
        self.leader_checkpoint(2)
        self.assertTrue(self.table_exists_on_follower(self.uri),
            'table should still be visible before drop is published')

        # Publish the drop and check that the table is removed from the next checkpoint.
        self.publish(self.uri, 30)
        self.set_stable_epoch(30)
        self.leader_checkpoint(3)
        self.assertFalse(self.table_exists_on_follower(self.uri),
            'table should be invisible after drop publish and checkpoint')

    #
    # Error handling tests for publish
    #

    def test_publish_error_zero_epoch(self):
        """
        Cannot publish with zero schema epoch.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.publish(self.uri, 'disaggregated=(schema_epoch=0)'),
            '/zero not permitted/')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
                                ',oldest_timestamp=' + self.timestamp_str(1))

    def test_publish_error_epoch_not_newer_than_stable(self):
        """
        The schema epoch supplied to publish must be newer than the current stable disaggregated
        schema epoch.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.set_stable_epoch(10)

        # Epoch equal to stable must fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.publish(self.uri, 10),
            '/Cannot publish with a schema epoch that is older/')
        # Epoch older than stable must fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.publish(self.uri, 5),
            '/Cannot publish with a schema epoch that is older/')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
                                ',oldest_timestamp=' + self.timestamp_str(1))

    def test_publish_error_invalid_uri(self):
        """
        Calling publish with a non-table URI returns EINVAL.
        """
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.publish(
                f'file:{self.test_name}_err_uri',
                'disaggregated=(schema_epoch=1)'),
            '/only supported for table: and layered:/')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
                                ',oldest_timestamp=' + self.timestamp_str(1))

    def subprocess_checkpoint_fails_without_publish(self):
        """
        Helper run in a subprocess by test_checkpoint_fails_without_publish.
        Triggers a panic by checkpointing data for an unpublished table.
        """
        self.set_stable_epoch(5)
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Write data so the underlying file becomes dirty and receives a real
        # (non-fake) block checkpoint, which triggers the shared metadata UPDATE.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        # Checkpoint without publishing: panics because the shared metadata UPDATE
        # finds no existing CREATE entry for this unpublished table.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        try:
            self.session.checkpoint()  # Expected to panic.
        except wiredtiger.WiredTigerError:
            # Exit immediately to avoid hanging in tearDown when closing a panicked connection.
            os._exit(1)

    def test_checkpoint_fails_without_publish(self):
        """
        Once data has been written to a table that has not been published, taking a
        checkpoint must panic. The block checkpoint resolve for the underlying table
        file enqueues a shared metadata UPDATE, which requires an existing CREATE
        entry. That entry only exists after the CREATE metadata operation is applied,
        which requires the table to be published with a matching schema epoch.

        Checkpoint panics on this error, so the trigger is run in a subprocess.
        """
        subdir = 'SUBPROCESS'
        [returncode, _] = self.run_subprocess_function(subdir,
            f'{self.test_name}.{self.test_name}.subprocess_checkpoint_fails_without_publish',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on checkpoint of unpublished table')
        # Set timestamps so the tearDown shutdown checkpoint can succeed (precise_checkpoint=true
        # requires a stable timestamp).
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))

    def subprocess_checkpoint_fails_with_publish_at_later_epoch(self):
        """
        Helper run in a subprocess by test_checkpoint_fails_with_publish_at_later_epoch.
        Triggers a panic by checkpointing data for a table whose CREATE is deferred because
        it was published at a higher schema epoch than the current stable epoch.
        """
        self.set_stable_epoch(5)
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Publish at epoch 10, which is ahead of the stable epoch (5). The CREATE entry
        # in the metadata queue will be assigned epoch 10 and therefore deferred when the
        # checkpoint runs at the stable epoch 5.
        self.publish(self.uri, 10)

        # Write data so the underlying file becomes dirty and receives a real
        # (non-fake) block checkpoint, which triggers the shared metadata UPDATE.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        # Checkpoint at stable epoch 5: the CREATE (published at epoch 10) is deferred,
        # but the block checkpoint enqueues an UPDATE at epoch 5. The UPDATE is processed,
        # detects the deferred CREATE at a higher epoch, and panics.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        try:
            self.session.checkpoint()  # Expected to panic.
        except wiredtiger.WiredTigerError:
            # Exit immediately to avoid hanging in tearDown when closing a panicked connection.
            os._exit(1)

    def test_checkpoint_fails_with_publish_at_later_epoch(self):
        """
        Once data has been written to a table that was published at a higher schema epoch
        than the current stable epoch, taking a checkpoint must panic. The CREATE entry is
        deferred (its epoch exceeds the stable epoch), so no shared metadata entry exists
        when the block checkpoint tries to apply the UPDATE for stable data.

        Checkpoint panics on this error, so the trigger is run in a subprocess.
        """
        subdir = 'SUBPROCESS_LATER_EPOCH'
        [returncode, _] = self.run_subprocess_function(subdir,
            f'{self.test_name}.{self.test_name}.'
            'subprocess_checkpoint_fails_with_publish_at_later_epoch',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on checkpoint of table published at a later epoch')
        # Set timestamps so the tearDown shutdown checkpoint can succeed (precise_checkpoint=true
        # requires a stable timestamp).
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))

    #
    # Statistics tests
    #

    def test_publish_stats(self):
        """
        Test the publish statistics.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # No schema_epoch in config: no-op, returns success.
        self.session.publish(self.uri, '')
        self.assertStatEqualSoon(stat.conn.session_table_publish_success, 1)
        self.assertStatEqualSoon(stat.conn.session_table_publish_fail, 0)

        # Publish create with a valid epoch: success stat increments.
        self.publish(self.uri, 10)
        self.assertStatEqualSoon(stat.conn.session_table_publish_success, 2)
        self.assertStatEqualSoon(stat.conn.session_table_publish_fail, 0)

        # Checkpoint with stable schema epoch lower than the published epoch: the operation is
        # counted as unstable and deferred to the next checkpoint.
        self.set_stable_epoch(5)
        self.leader_checkpoint(1)
        self.assertStatEqualSoon(stat.conn.checkpoint_disagg_metadata_unstable, 1)
        self.assertStatEqualSoon(stat.conn.checkpoint_disagg_metadata_apply, 0)

        # Checkpoint with stable schema epoch matching the published epoch: the operation is applied.
        self.set_stable_epoch(10)
        self.leader_checkpoint(2)
        self.assertStatEqualSoon(stat.conn.checkpoint_disagg_metadata_unstable, 1)
        self.assertStatEqualSoon(stat.conn.checkpoint_disagg_metadata_apply, 1)

        # Publish drop with a valid epoch: success stat increments.
        self.session.drop(self.uri)
        self.publish(self.uri, 20)
        self.assertStatEqualSoon(stat.conn.session_table_publish_success, 3)
        self.assertStatEqualSoon(stat.conn.session_table_publish_fail, 0)

        # Zero epoch: returns EINVAL (fail stat increments).
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.publish(self.uri, 'disaggregated=(schema_epoch=0)'),
            '/zero not permitted/')
        self.assertStatEqualSoon(stat.conn.session_table_publish_success, 3)
        self.assertStatEqualSoon(stat.conn.session_table_publish_fail, 1)
