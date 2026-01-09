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
# test_prepare_discover07.py
#   Test layered cursor with prepared tombstone transaction discovery in disaggregated storage:
#   - Setup layered cursor as leader, insert and commit data
#   - Prepare transaction, advance stable timestamp, checkpoint
#   - Reopen as follower and pickup checkpoint, verify committed data visibility
#   - Step up as leader, discover and claim prepared transaction, commit and verify

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_prepare_discover07(wttest.WiredTigerTestCase):
    tablename = 'test_prepare_discover07'
    uri = 'layered:' + tablename

    resolve_scenarios = [
        # FIXME-WT-15051 handle searching for committed prepared tombstone on standby
        # ('commit', dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]
    # Use disaggregated storage scenarios
    disagg_storages = gen_disagg_storages('test_prepare_discover07', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolve_scenarios)

    # Base configuration for leader connection
    conn_base_config = 'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def test_prepare_discover_layered(self):
        # Step 1: Setup layered cursor as leader and insert initial committed data
        self.pr("=== Phase 1: Setup layered cursor as leader ===")

        # Set initial timestamps
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        # Create the layered table
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # Insert some initial data and commit at timestamp 60
        self.session.begin_transaction()
        cursor[1] = "committed_value_1"
        cursor[2] = "committed_value_2"
        cursor[3] = "committed_value_3"
        cursor[4] = "committed_value_4"
        cursor[5] = "committed_value_5"
        cursor[6] = "committed_value_6"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        # Advance stable timestamp to include committed data
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        # Verify committed data can be read
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(cursor[1], "committed_value_1")
        self.assertEqual(cursor[2], "committed_value_2")
        self.assertEqual(cursor[3], "committed_value_3")
        self.assertEqual(cursor[4], "committed_value_4")
        self.assertEqual(cursor[5], "committed_value_5")
        self.assertEqual(cursor[6], "committed_value_6")
        self.session.rollback_transaction()

        # Step 2: Prepare transaction and create checkpoint

        # Insert more data and prepare the transaction at timestamp 100
        self.session.begin_transaction()
        for i in range(4, 7):
            cursor.set_key(i)
            self.assertEqual(cursor.remove(), 0)

        # Prepare with prepared_id 123 at timestamp 100
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                       ',prepared_id=' + self.prepared_id_str(123))

        # Advance stable timestamp past the prepare timestamp
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        # Create checkpoint to persist the prepared transaction
        checkpoint_session = self.conn.open_session()
        checkpoint_session.checkpoint()
        checkpoint_session.close()

        # Step 3: Reopen as follower and pickup checkpoint

        # Get the checkpoint metadata before closing
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Configure as follower with checkpoint pickup (not using backup)
        follower_config = self.conn_base_config + \
                         'disaggregated=(role="follower",' + \
                         f'checkpoint_meta="{checkpoint_meta}")'
        # Reopen connection as follower
        self.reopen_conn(config=follower_config)

        # Verify committed data is visible but prepared data is not
        cursor = self.session.open_cursor(self.uri)

        # Should see committed data at timestamp 60
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(cursor[1], "committed_value_1")
        self.assertEqual(cursor[2], "committed_value_2")
        self.assertEqual(cursor[3], "committed_value_3")
        self.assertEqual(cursor[4], "committed_value_4")
        self.assertEqual(cursor[5], "committed_value_5")
        self.assertEqual(cursor[6], "committed_value_6")
        self.session.rollback_transaction()

        cursor.close()

        # Open prepared discover cursor to find pending prepared transactions
        prepared_discover_cursor = self.session.open_cursor("prepared_discover:")

        # Walk through prepared discover cursor to find our transaction
        discover_session = self.conn.open_session()
        count = 0
        found_prepared_id = None

        while prepared_discover_cursor.next() == 0:
            count += 1
            prepared_id = prepared_discover_cursor.get_key()
            self.assertEqual(prepared_id, 123)  # Should find our prepared transaction
            found_prepared_id = prepared_id

            # Claim the prepared transaction
            discover_session.begin_transaction("claim_prepared_id=" + self.prepared_id_str(prepared_id))
            break

        self.assertEqual(count, 1)  # Should find exactly one prepared transaction
        self.assertEqual(found_prepared_id, 123)
        prepared_discover_cursor.close()
        if self.commit:
            # Commit the claimed prepared transaction at timestamp 200
            discover_session.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                            ",durable_timestamp=" + self.timestamp_str(210))
        else:
            discover_session.rollback_transaction("rollback_timestamp=" + self.timestamp_str(210))
        discover_session.close()

        # Update stable timestamp to include the committed transaction
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(220))

        # Verify all data
        read_session = self.conn.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # Check committed data is still visible at timestamp 60
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(read_cursor[1], "committed_value_1")
        self.assertEqual(read_cursor[2], "committed_value_2")
        self.assertEqual(read_cursor[3], "committed_value_3")
        self.assertEqual(read_cursor[4], "committed_value_4")
        self.assertEqual(read_cursor[5], "committed_value_5")
        self.assertEqual(read_cursor[6], "committed_value_6")

        read_session.rollback_transaction()

        # Check all data is visible at timestamp 200
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        self.assertEqual(read_cursor[1], "committed_value_1")
        self.assertEqual(read_cursor[2], "committed_value_2")
        self.assertEqual(read_cursor[3], "committed_value_3")
        self.session.breakpoint()
        for i in range(4, 7):
            read_cursor.set_key(i)
            if self.commit:
                self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
            else:
                self.assertEqual(0, read_cursor.search())
                self.assertEqual(f'committed_value_{i}', read_cursor.get_value())
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

