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

# Test eviction free updates with prepared transactions

class test_prepare37(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare37'

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_commit_prepare(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Step 1: Insert a base value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_1")
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Update the value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_2")
            cursor_committed.update()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor_committed.close()

        # Step 3: Update key 20 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(20)
        cursor_prepare.set_value("prepared_value_20_3")
        cursor_prepare.insert()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Step 4: Commit prepared transaction
        session_prepare.commit_transaction("commit_timestamp=" + self.timestamp_str(40) + ',durable_timestamp=' + self.timestamp_str(45))
        session_prepare.close()

        # Step 5: Force eviction to trigger reconciliation with the prepared data
        # This ensures the prepared update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(45))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "prepared_value_20_3")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

        # Make durable timestamp of the prepared transaction stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(45))

        # Step 6: Force eviction to trigger reconciliation
        # This ensures the committed update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(45))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "prepared_value_20_3")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_rollback_prepare(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Step 1: Insert a base value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_1")
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Update the value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_2")
            cursor_committed.update()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor_committed.close()

        # Step 3: Update key 20 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(20)
        cursor_prepare.set_value("prepared_value_20_3")
        cursor_prepare.insert()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Step 4: Rollback prepared transaction
        session_prepare.rollback_transaction("rollback_timestamp=" + self.timestamp_str(40))
        session_prepare.close()

        # Step 5: Force eviction to trigger reconciliation with the prepared data
        # This ensures the prepared update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(40))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

        # Make rollback timestamp of the prepared transaction stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Step 6: Force eviction to trigger reconciliation
        # This ensures the committed update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(40))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_commit_prepare_delete(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Step 1: Insert a base value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_1")
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Update the value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_2")
            cursor_committed.update()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor_committed.close()

        # Step 3: Delete key 20 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(20)
        cursor_prepare.remove()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Step 4: Commit prepared transaction
        session_prepare.commit_transaction("commit_timestamp=" + self.timestamp_str(40) + ',durable_timestamp=' + self.timestamp_str(45))
        session_prepare.close()

        # Step 5: Force eviction to trigger reconciliation with the prepared data
        # This ensures the prepared update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 20):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(45))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        read_cursor.set_key(20)
        self.assertEqual(read_cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))

        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

        # Make durable timestamp of the prepared transaction stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(45))

        # Step 6: Force eviction to trigger reconciliation
        # This ensures the committed update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 20):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(45))
        read_cursor.set_key(20)
        self.assertEqual(read_cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_rollback_prepare_delete(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Step 1: Insert a base value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_1")
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Update the value
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i) + "_2")
            cursor_committed.update()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor_committed.close()

        # Step 3: Delete key 20 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(20)
        cursor_prepare.remove()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Step 4: Rollback prepared transaction
        session_prepare.rollback_transaction("rollback_timestamp=" + self.timestamp_str(40))
        session_prepare.close()

        # Step 5: Force eviction to trigger reconciliation with the prepared data
        # This ensures the prepared update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(40))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()

        # Make rollback timestamp of the prepared transaction stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Step 6: Force eviction to trigger reconciliation
        # This ensures the committed update gets written to disk storage
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 21):  # Evict committed data pages
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Verify key 20 is visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(40))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(30))
        self.assertEqual(read_cursor[20], "committed_value_20_2")
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(25))
        self.assertEqual(read_cursor[20], "committed_value_20_1")
        self.session.rollback_transaction()
