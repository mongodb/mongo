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

import wiredtiger, wttest
from prepare_util import test_prepare_preserve_prepare_base

# Test prepared transaction rollback behavior and history store interactions.
# This test verifies that the history store correctly writes data to disk for rolled back
# prepare transactions with precise checkpoint.

WT_TS_MAX = 2**64-1

class test_prepare39(test_prepare_preserve_prepare_base):

    conn_config = 'precise_checkpoint=true,preserve_prepared=true,statistics=(all)'
    uri = 'table:test_prepare39'

    def check_ckpt_hs(self, expected_hs_value, expected_hs_start_ts,
                      expected_hs_stop_ts, expect_prepared_in_datastore = False):
        session = self.conn.open_session(self.session_config)
        session.checkpoint()
        count = 0
        # Check the history store file value.
        cursor = session.open_cursor("file:WiredTigerHS.wt", None, 'checkpoint=WiredTigerCheckpoint')
        for _, _, hs_start_ts, _, hs_stop_ts, _, type, value in cursor:
            # check that the update type is WT_UPDATE_STANDARD
            self.assertEqual(type, 3)
            self.assertEqual(hs_start_ts, expected_hs_start_ts)
            self.assertEqual(hs_stop_ts, expected_hs_stop_ts)
            self.assertEqual(value.decode(), expected_hs_value+'\x00')
            count = count+1
        self.assertGreaterEqual(count, 1)
        cursor.close()
        session.close()

    def verify_read_data(self, read_ts, key, expected_value):
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(read_ts))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        read_cursor.set_key(key)
        self.assertEqual(read_cursor.search(), 0)
        self.assertEqual(read_cursor.get_value(), expected_value)
        self.session.rollback_transaction()

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_hs_rollback_prepare(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        value_a = 'aaaaaaa'
        value_b = 'bbbbbbb'
        # Step 1: Insert 2 updates and commit for keys 1-20
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 22):
            cursor_committed.set_key(i)
            cursor_committed.set_value(value_a)
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(21))
        cursor_committed.close()

        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 22):
            cursor_committed.set_key(i)
            cursor_committed.set_value(value_b)
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Create prepared transaction for key 21 with prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(21)
        cursor_prepare.set_value("prepared_value_21")
        cursor_prepare.insert()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Now rollback the update
        session_prepare.rollback_transaction('rollback_timestamp=' + self.timestamp_str(35))
        session_prepare.close()

        # Move stable timestamp and do another checkpoint where rollback ts is still not stable,
        # Check that the update in the history store has max stop_ts
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.check_ckpt_hs(value_b, 25, WT_TS_MAX)

        # Move stable timestamp to after rollback ts and checkpoint
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.session.checkpoint()

        # Step 3: Force eviction to trigger reconciliation with the prepared data
        # This ensures the update gets written to disk
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()

        # Check that we still read the correct data from different timestamp
        self.verify_read_data(21, 21, value_a)
        self.verify_read_data(25, 21, value_b)
        self.verify_read_data(30, 21, value_b)
        self.verify_read_data(35, 21, value_b)

        self.conn.close()

        # Reopen the database and read from the HS file stored on disk
        # Since the prepared update is rolled back, value_b is stored in the data store so value_a is in the history store
        conn_backup = self.wiredtiger_open(self.home)
        backup_session = conn_backup.open_session(self.session_config)
        backup_session.begin_transaction('read_timestamp='+ self.timestamp_str(10))
        cursor = backup_session.open_cursor("file:WiredTigerHS.wt", None,None)
        count = 0
        for _, _, hs_start_ts, _, hs_stop_ts, _, type, value in cursor:
            # WT_UPDATE_STANDARD
            self.assertEqual(type, 3)
            self.assertEqual(hs_start_ts, 21)
            self.assertEqual(hs_stop_ts, 25)
            self.assertEqual(value.decode(), value_a+'\x00')
            count = count + 1
        self.assertGreaterEqual(count, 1)
        cursor.close()
        backup_session.rollback_transaction()
        backup_session.close()
