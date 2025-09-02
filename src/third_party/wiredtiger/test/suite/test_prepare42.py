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

# Test prepare insert rollback to delete the key with a rollback tombstone

class test_prepare42(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare42'

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_prepare_insert_rollback(self):
        # Setup: Initialize timestamps with stable < prepare timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Insert a value and commit for keys 1-19
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 20):
            cursor.set_key(i)
            cursor.set_value("commit_value")
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(21))

        # Insret key 20 with a prepared update prepared_id=1
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare.set_key(20)
        cursor_prepare.set_value("prepared_value_20_3")
        cursor_prepare.insert()
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35)+',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()

        # Rollback the prepared transaction
        session_prepare.rollback_transaction("rollback_timestamp=" + self.timestamp_str(45))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Make stable timestamp equal to prepare timestamp - this should allow checkpoint to reconcile prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Force the page to be evicted
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 20):
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()

        # Make stable timestamp equal to rollback timestamp - this should delete the prepared update
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(45))

        # Verify checkpoint writes prepared time window to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Force the page to be evicted again
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 20):
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()

        # Verify the key is gone
        cursor.set_key(20)
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
