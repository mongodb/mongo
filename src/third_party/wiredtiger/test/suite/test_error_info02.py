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

import time, wiredtiger
from error_info_util import error_info_util

# test_error_info02.py
#   Test that the get_last_error() session API returns the last error for rollback error to
#   occur in the session.
class test_error_info02(error_info_util):
    uri = "table:test_error_info.wt"

    def test_wt_rollback_cache_overflow(self):
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

        # Start a transaction and insert a value large enough to trigger eviction app worker
        # threads. Loop 100 times to ensure that the eviction server is busy evicting, and the
        # cache will be full when the application worker thread checks if eviction is needed.
        for i in range(100):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.set_value(str(i)*1024*5000)

            try:
                cursor.update()
            except wiredtiger.WiredTigerError as e:
                if wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e):
                    break
                else:
                    raise e

            self.session.commit_transaction()

        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_CACHE_OVERFLOW, "Cache capacity has overflown")

        self.ignoreStdoutPatternIfExists("Cache capacity has overflown")

    def test_wt_rollback_write_conflict_update_list(self):
        """
        Try to write conflicting data with two threads. There will be an update in the update list
        that is not visible to the second session, therefore, it cannot modify it.
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
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor2.update(), wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))

        # Expect the get_last_error reason to give us the true reason for the rollback.
        # The error will be set in the second session.
        self.session = session2
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_WRITE_CONFLICT, "Write conflict between concurrent operations")

    def test_wt_rollback_write_conflict_time_start(self):
        """
        Try to write conflicting data with two threads. There will be an update from the first
        session that is written to disk, and it is not visible to second session, so there is a
        conflict.
        """
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor1 = self.session.open_cursor(self.uri)
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None, "debug=(release_evict)")

        # Begin a transaction on the first session and set a key and value.
        self.session.begin_transaction()
        cursor1.set_key('key')
        cursor1.set_value('value')

        # Insert a key and value within a transaction in the second session.
        session2.begin_transaction()
        cursor2.set_key('key')
        cursor2.set_value('abc')
        cursor2.update()
        session2.commit_transaction()

        # Evict the page from in-memory and write the page to disk.
        cursor2.set_key('key')
        cursor2.search()
        cursor2.reset()

        # Because the first session started a transaction earlier than the second session, the
        # update made by the second session will be invisible to the first session.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor1.insert(), wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_WRITE_CONFLICT, "Write conflict between concurrent operations")

    def test_wt_rollback_write_conflict_time_stop(self):
        """
        Try to write conflicting data with two threads. There will be an overwrite update from the
        first session that is written to disk, and it is not visible to the second session, so
        there is a conflict.
        """
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor1 = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None, "debug=(release_evict)")

        # Insert a key and value.
        self.session.begin_transaction()
        cursor1.set_key('key')
        cursor1.set_value('abc')
        cursor1.insert()
        self.session.commit_transaction()

        # Evict page from in-memory and write to disk.
        cursor1.set_key('key')
        cursor1.search()
        cursor1.reset()

        # Begin a transaction to update the same key on the first session.
        self.session.begin_transaction()
        cursor1.set_key('key')
        cursor1.set_value('value')

        # Overwrite the same key within a transaction on the second session.
        session2.begin_transaction()
        cursor2.set_key('key')
        cursor2.remove()
        session2.commit_transaction()

        # Evict page from in-memory and write to disk.
        cursor2.set_key('key')
        cursor2.search()
        cursor2.reset()

        # Because the first session started a transaction earlier than the second session, the
        # overwrite made by the second session will be invisible to the first session.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor1.insert(), wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_WRITE_CONFLICT, "Write conflict between concurrent operations")

    def test_wt_rollback_oldest_for_eviction(self):
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
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.update(), wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))
        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_OLDEST_FOR_EVICTION, "Transaction has the oldest pinned transaction ID")
