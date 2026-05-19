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

# test_layered104.py
#   Test WT_SESSION::publish for disaggregated storage.
#
#   Schema operations (create, drop) on a leader do not get included in the next checkpoint
#   until they are published with a schema epoch.

import time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

#   Test WT_SESSION::publish for disaggregated storage.
@disagg_test_class
class test_layered104(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = 'layered:test_layered104'

    disagg_storages = gen_disagg_storages('test_layered104', disagg_only=True)
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

    def get_stat(self, stat_name):
        """
        Get the value of a statistic by name.
        """
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_name][2]
        stat_cursor.close()
        return value

    def assertStatEqual(self, stat_name, expected_value, retries=10):
        """
        Assert that a statistic has the expected value, retrying if necessary.
        """
        # Stats may be updated asynchronously, so retry a few times if the expected value is not
        # observed.
        for attempt in range(retries):
            value = self.get_stat(stat_name)
            if value == expected_value:
                return
            if attempt < retries - 1:
                time.sleep(0.1)
        self.assertEqual(value, expected_value)

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
                'file:test_layered104_err_uri',
                'disaggregated=(schema_epoch=1)'),
            '/only supported for table: and layered:/')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
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
        self.assertStatEqual(stat.conn.session_table_publish_success, 1)
        self.assertStatEqual(stat.conn.session_table_publish_fail, 0)

        # Publish create with a valid epoch: success stat increments.
        self.publish(self.uri, 10)
        self.assertStatEqual(stat.conn.session_table_publish_success, 2)
        self.assertStatEqual(stat.conn.session_table_publish_fail, 0)

        # Checkpoint with stable schema epoch lower than the published epoch: the operation is
        # counted as unstable and deferred to the next checkpoint.
        self.set_stable_epoch(5)
        self.leader_checkpoint(1)
        self.assertStatEqual(stat.conn.checkpoint_disagg_metadata_unstable, 1)
        self.assertStatEqual(stat.conn.checkpoint_disagg_metadata_apply, 0)

        # Checkpoint with stable schema epoch matching the published epoch: the operation is applied.
        self.set_stable_epoch(10)
        self.leader_checkpoint(2)
        self.assertStatEqual(stat.conn.checkpoint_disagg_metadata_unstable, 1)
        self.assertStatEqual(stat.conn.checkpoint_disagg_metadata_apply, 1)

        # Publish drop with a valid epoch: success stat increments.
        self.session.drop(self.uri)
        self.publish(self.uri, 20)
        self.assertStatEqual(stat.conn.session_table_publish_success, 3)
        self.assertStatEqual(stat.conn.session_table_publish_fail, 0)

        # Zero epoch: returns EINVAL (fail stat increments).
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.publish(self.uri, 'disaggregated=(schema_epoch=0)'),
            '/zero not permitted/')
        self.assertStatEqual(stat.conn.session_table_publish_success, 3)
        self.assertStatEqual(stat.conn.session_table_publish_fail, 1)
