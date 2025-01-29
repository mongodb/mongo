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

import time
import wiredtiger
import wttest

# test_error_info02.py
#   Test that the get_last_error() session API returns the last error for rollback error to
#   occur in the session.
class test_error_info02(wttest.WiredTigerTestCase):
    uri = "table:test_error_info.wt"

    def assert_error_equal(self, err_val, sub_level_err_val, err_msg_val):
        err, sub_level_err, err_msg = self.session.get_last_error()
        self.assertEqual(err, err_val)
        self.assertEqual(sub_level_err, sub_level_err_val)
        self.assertEqual(err_msg, err_msg_val)

    def api_call_with_wt_rollback_wt_cache_overflow(self):
        """
        Try to insert a key value pair with an unreasonably low cache max wait time and
        application worker threads are attempting to do eviction.
        """
        # Configure the connection with an unrealistically small cache_max_wait_ms value and
        # a very low eviction trigger threshold.
        self.conn.reconfigure('cache_max_wait_ms=2,eviction_dirty_target=1,eviction_dirty_trigger=2')

        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Open a session and cursor.
        cursor = self.session.open_cursor(self.uri)

        # Start a transaction and insert a value large enough to trigger eviction app worker threads.
        self.session.begin_transaction()
        cursor.set_key("key_a")
        cursor.set_value("a"*1024*5000)
        cursor.update()
        self.session.commit_transaction()

        # Start a new transaction and attempt to insert a value. The very low cache_max_wait_ms
        # value should cause the eviction thread to time out.
        self.session.begin_transaction()
        cursor.set_key("key_b")
        cursor.set_value("b")

        # This reason is the default reason for WT_ROLLBACK errors so we need to catch it.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.update())

        self.ignoreStdoutPatternIfExists("transaction rolled back because of cache overflow")

    def api_call_with_wt_rollback_wt_write_conflict(self):
        """
        Try to write conflicting data with two threads.
        """
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Insert a key and value.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor.set_key("key")
        cursor.set_value("value")
        cursor.update()
        self.session.commit_transaction()
        self.session.checkpoint()
        cursor.close()

        # Update the key in the first session.
        session1 = self.session
        cursor1 = session1.open_cursor(self.uri)
        session1.begin_transaction()
        cursor1["key"] = "aaa"

        # Insert the same key in the second session, expect a conflict error to be produced.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri)
        session2.begin_transaction()
        cursor2.set_key("key")
        cursor2.set_value("bbb")

        # Catch the default reason for WT_ROLLBACK errors.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor2.update())

        # Expect the get_last_error reason to give us the true reason for the rollback.
        # The error will be set in the second session.
        self.session = session2

    def api_call_with_wt_rollback_wt__oldest_for_eviction(self):
        """
        Try to insert a key value pair while the cache is full.
        """
        # Configure the connection with the min cache size.
        self.conn.reconfigure('cache_size=1MB')

        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri)

        # Start a new transaction and insert a value far too large for cache.
        self.session.begin_transaction()
        cursor.set_key("key_a")
        cursor.set_value("a"*1024*5000)
        self.assertEqual(0, cursor.update())

        # Let WiredTiger's accounting catch up.
        time.sleep(2)

        # Attempt to insert another value with the same transaction. This will result in the
        # application thread being pulled into eviction and getting rolled back.
        cursor.set_key("key_b")
        cursor.set_value("b"*1024)

        # Catch the default reason for WT_ROLLBACK errors.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.update())

    def test_wt_rollback_wt_cache_overflow(self):
        # FIXME-WT-14046
        self.skipTest("FIXME-WT-14046")
        self.api_call_with_wt_rollback_wt_cache_overflow()
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_CACHE_OVERFLOW, "Cache capacity has overflown")

    def test_wt_rollback_wt_write_conflict(self):
        self.api_call_with_wt_rollback_wt_write_conflict()
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_WRITE_CONFLICT, "Write conflict between concurrent operations")

    def test_wt_rollback_wt_oldest_for_eviction(self):
        self.api_call_with_wt_rollback_wt__oldest_for_eviction()
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_OLDEST_FOR_EVICTION, "Transaction has the oldest pinned transaction ID")
