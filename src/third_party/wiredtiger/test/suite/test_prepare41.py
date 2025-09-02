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

# Test update restore prepared updates retains full updates for modifies.

class test_prepare41(test_prepare_preserve_prepare_base):

    conn_config = 'precise_checkpoint=true,preserve_prepared=true,statistics=(all)'
    uri = 'table:test_prepare41'

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_prepare_update(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        value = 'a' * 100
        # Step 1: Insert a value and commit for keys 1-20
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor.set_key(i)
            cursor.set_value(value)
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(21))

        # Step 2: do a modify for key 20
        self.session.begin_transaction()
        cursor.set_key(20)
        modifications = [wiredtiger.Modify('b', 0, 1)]  # Modify 'a' * 100 to `b` + a * 99
        cursor.modify(modifications)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))

        # Step 3: Create prepared transaction for key 20 with prepared_id=1
        self.session.begin_transaction()
        cursor.set_key(20)
        modifications = [wiredtiger.Modify('d', 0, 1)]  # Modify 'a' * 100 to `d` + a * 99
        cursor.modify(modifications)
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30)+',prepared_id=' + self.prepared_id_str(1))
        cursor.reset()

        # Make stable timestamp equal to prepare timestamp - this should allow prepared update to be evicted
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(35))

        # Step 4: Force eviction to trigger reconciliation with the prepared data
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

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        # Step 5: Do a checkpoint to insert updates to the history store again.
        self.session.checkpoint()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        self.assertEqual(cursor[20], 'b' + 'a' * 99)

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_prepare_delete(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        value = 'a' * 100
        # Step 1: Insert a value and commit for keys 1-20
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):
            cursor.set_key(i)
            cursor.set_value(value)
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(21))

        # Step 2: do a modify for key 20
        self.session.begin_transaction()
        cursor.set_key(20)
        modifications = [wiredtiger.Modify('b', 0, 1)]  # Modify 'a' * 100 to `b` + a * 99
        cursor.modify(modifications)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))

        # Step 3: Delete key 20 with prepared_id=1
        self.session.begin_transaction()
        cursor.set_key(20)
        cursor.remove()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30)+',prepared_id=' + self.prepared_id_str(1))
        cursor.reset()

        # Make stable timestamp equal to prepare timestamp - this should allow prepared update to be evicted
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(35))

        # Step 4: Force eviction to trigger reconciliation with the prepared data
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

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        # Step 5: Do a checkpoint to insert updates to the history store again.
        self.session.checkpoint()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        self.assertEqual(cursor[20], 'b' + 'a' * 99)
