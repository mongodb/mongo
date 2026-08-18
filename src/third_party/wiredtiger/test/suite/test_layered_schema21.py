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

# Dropping a published but uncheckpointed layered table would discard the only copy of its
# committed data, since the published CREATE obligates a covering checkpoint to include that
# data. The drop must be refused until a checkpoint publishes the table.

import errno
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema21(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,cache_cursors=false,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S'
    nrows = 100

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def create_and_publish_table(self):
        """Create and publish a table under a stable schema epoch so it awaits publication."""
        # Precise checkpoint requires a stable timestamp at connection close.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 10)

    def write_rows(self, commit_ts=None):
        """Write nrows to the table, then commit at commit_ts or roll back if it is None."""
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = 'value'
        cursor.close()
        if commit_ts is None:
            self.session.rollback_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def assert_all_rows(self, session):
        """Assert that every written row is readable through the given session."""
        cursor = session.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            self.assertEqual(cursor.get_value(), 'value')
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)

    def assert_drop_refused(self):
        """Assert that dropping the table is refused with EBUSY / WT_DIRTY_DATA."""
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.uri))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertEqual(sub, wiredtiger.WT_DIRTY_DATA)
        self.assertTrue('unpublished data' in msg)

    def truncate_all_rows(self, commit_ts):
        """Truncate the whole key range at commit_ts."""
        c_start = self.session.open_cursor(self.uri)
        c_start.set_key(1)
        c_stop = self.session.open_cursor(self.uri)
        c_stop.set_key(self.nrows)
        self.session.begin_transaction()
        self.session.truncate(None, c_start, c_stop, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        c_start.close()
        c_stop.close()

    def assert_no_rows(self, session):
        """Assert that the table reads back empty through the given session."""
        cursor = session.open_cursor(self.uri)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def assert_drop_succeeds(self):
        """Drop the table and confirm it is gone from local metadata."""
        self.session.drop(self.uri)
        self.assertFalse(self.uri_in_local_metadata(self.conn, self.uri))

    #
    # Drop lifecycle tests
    #

    def test_drop_with_committed_data_is_refused(self):
        # A table that awaits publication and holds committed data keeps its only copy in memory,
        # so the drop is refused until a checkpoint publishes it.
        self.create_and_publish_table()
        self.write_rows(commit_ts=3)
        self.assert_drop_refused()

        # The refused drop left no partial state, so the committed rows remain readable.
        self.assert_all_rows(self.session)

        # A checkpoint publishes and persists the table with no data loss: the rows reach a
        # follower, and the published table then drops normally.
        self.set_stable_epoch(10)
        self.leader_checkpoint(3)
        conn_follower, session_follower = self.open_follower()
        self.assert_all_rows(session_follower)
        session_follower.close()
        conn_follower.close()
        self.assert_drop_succeeds()

    def test_checkpoint_between_create_and_drop_epochs_keeps_data(self):
        # Create and publish the table at schema epoch 5, then write committed data at timestamp 10.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)
        self.write_rows(commit_ts=10)

        # A later checkpoint at schema epoch 6 covers the create but not a drop published at epoch
        # 7, so it must include the table with the timestamp-10 writes. Dropping the table now would
        # discard those writes when the handle closes, so the drop is refused first.
        self.assert_drop_refused()

        # The checkpoint at schema epoch 6 and timestamp 11 includes the table and its data.
        self.set_stable_epoch(6)
        self.leader_checkpoint(11)
        conn_follower, session_follower = self.open_follower()
        self.assert_all_rows(session_follower)
        session_follower.close()
        conn_follower.close()

    def test_drop_published_checkpointed_with_dirty_data(self):
        # Create and publish at epoch 5.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)

        # Write and checkpoint at ts 10, epoch 6. Epoch 6 > publish epoch 5, so
        # the checkpoint covers the create and clears AWAITS_PUBLISH.
        self.write_rows(commit_ts=10)
        self.set_stable_epoch(6)
        self.leader_checkpoint(10)

        # Write new data at ts 11 -- uncheckpointed. AWAITS_PUBLISH is now clear.
        self.write_rows(commit_ts=11)

        # Drop at epoch 7. The published table no longer awaits publication, so the
        # unpublished-data guard does not apply. The drop is still refused with EBUSY
        # because closing the dirty ingest file fails, protecting the ts-11 data
        # through the same path a regular table uses.
        self.set_stable_epoch(7)
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.uri))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertEqual(sub, wiredtiger.WT_DIRTY_DATA)
        self.assertTrue('dirty data' in msg)

        # Checkpoint to quiesce dirty state so teardown can close cleanly.
        self.set_stable_epoch(8)
        self.leader_checkpoint(11)

    def test_drop_with_drained_data_is_refused(self):
        # The step-up drain moves follower-era ingest rows into a stable constituent created
        # awaiting publication, so until a checkpoint publishes the table those rows exist only in
        # its cache and the drop must be refused, exactly as when the rows were committed directly.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)
        self.step_down()

        # A follower-era create and insert: the rows live only in the ingest constituent.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)
        self.write_rows(commit_ts=3)

        # Step up: the missing stable constituent is created awaiting publication and the drain
        # moves the rows into it.
        self.step_up()
        self.assert_drop_refused()

        # The refused drop left no partial state, so the committed rows remain readable.
        self.assert_all_rows(self.session)

        # A checkpoint publishes and persists the table with no data loss: the rows reach a
        # follower, and the published table then drops normally.
        self.set_stable_epoch(10)
        self.leader_checkpoint(3)
        conn_follower, session_follower = self.open_follower()
        self.assert_all_rows(session_follower)
        session_follower.close()
        conn_follower.close()
        self.assert_drop_succeeds()

    def test_drop_empty_is_allowed(self):
        # An awaiting-publication table with no data is transient: nothing obligates a checkpoint,
        # so the drop is allowed.
        self.create_and_publish_table()
        self.assert_drop_succeeds()

    def test_drop_rolled_back_data_is_allowed(self):
        # Rolled-back writes leave no committed durable data, so the drop is allowed.
        self.create_and_publish_table()
        self.write_rows()
        self.assert_drop_succeeds()

    def test_drop_after_checkpoint_is_allowed(self):
        # Once a checkpoint publishes the table, its data is durable and the drop is a normal drop.
        self.create_and_publish_table()
        self.write_rows(commit_ts=3)
        self.set_stable_epoch(10)
        self.leader_checkpoint(3)
        self.assert_drop_succeeds()

    def test_drop_with_replayed_truncate_is_refused(self):
        # A follower-era truncate reaches the stable constituent through the step-up drain rather
        # than the commit path. The deletion it carries lives only in the awaiting-publication
        # table until a checkpoint publishes it, so the drop must be refused meanwhile and the
        # truncate must survive the publish.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)
        self.step_down()

        # A follower-era create, insert and truncate: everything lives in the ingest constituent
        # and the truncate is held in the follower truncate list.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 5)
        self.write_rows(commit_ts=3)
        self.truncate_all_rows(commit_ts=5)

        # Step up: the missing stable constituent is created awaiting publication and the drain
        # moves the truncated rows into it, interleaved with the recorded truncate.
        self.step_up()
        self.assert_drop_refused()

        # The refused drop left no partial state, so the truncate is still in effect.
        self.assert_no_rows(self.session)

        # A checkpoint publishes and persists the table: a follower sees the truncated table
        # rather than the pre-truncate rows, and the published table then drops normally.
        self.set_stable_epoch(10)
        self.leader_checkpoint(6)
        conn_follower, session_follower = self.open_follower()
        self.assert_no_rows(session_follower)
        session_follower.close()
        conn_follower.close()
        self.assert_drop_succeeds()
