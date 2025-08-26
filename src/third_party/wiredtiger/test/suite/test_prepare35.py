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

# Tests checkpoint behavior with prepared transactions, specifically:
# - Writing prepared updates to disk during checkpoint
# - Handling of rollback tombstones for aborted prepared transactions
# - Ensuring prepared updates can be written with stable timestamp validation

class test_prepare35(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare35'

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_committed_prepare(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Step 1: Insert committed baseline data for keys 1-20
        cursor_committed = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor_committed.set_key(i)
            cursor_committed.set_value("committed_value_" + str(i))
            cursor_committed.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_committed.close()

        # Step 2: Create first prepared transaction for key 21 with prepared_id=1
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

        # Step 3: Force eviction to trigger reconciliation with the prepared data
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

        # Step 4: Rollback the first prepared transaction
        # This prepends a globally visible tombstone
        session_prepare.rollback_transaction("rollback_timestamp=" + self.timestamp_str(35))
        session_prepare.close()

        # Verify key 21 is not visible
        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(40))
        read_cursor = self.session.open_cursor(self.uri, None, None)
        read_cursor.set_key(21)
        self.assertEqual(read_cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        # Step 5: Insert second prepared transaction to same key with different prepared_id
        session_prepare2 = self.conn.open_session()
        cursor_prepare2 = session_prepare2.open_cursor(self.uri)
        session_prepare2.begin_transaction()
        cursor_prepare2.set_key(21)
        cursor_prepare2.set_value("prepared_value_21")
        cursor_prepare2.insert()
        session_prepare2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(45)+',prepared_id=' + self.prepared_id_str(2))

        # Advance stable timestamp past the new prepare timestamp
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(47))
        cursor_prepare2.close()

        # Verify the second prepared update is also be written to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)
