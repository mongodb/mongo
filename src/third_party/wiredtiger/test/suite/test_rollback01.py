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
import time, wiredtiger, wttest

class test_rollback(wttest.WiredTigerTestCase):
    uri = "table:test_rollback.wt"

    @wttest.skip_for_hook("disagg", "disagg requires an additional condition to evict pages")
    def test_wt_rollback_cursor_next_no_retry(self):
        """
        Try to insert a key value pair while the cache is full, and verify cursor->next() calls
        result in a rollback but no retry is expected to be performed. The cursor should be
        unpositioned after the rollback.
        """
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Create a page and insert 100 keys.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(100):
            cursor.set_key("key_a" + str(i))
            cursor.set_value("a" * 1024 * 1)
            self.assertEqual(0, cursor.insert())
        self.session.commit_transaction()

        # Evict in-memory page onto disk.
        evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        for i in range(100):
            evict_cursor.set_key("key_a" + str(i))
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()

        # Position a read cursor in the middle.
        session2 = self.conn.open_session()
        read_cursor = session2.open_cursor(self.uri)
        read_cursor.set_key("key_a" + str(10))
        read_cursor.search_near()

        # Configure the connection with an unrealistically small cache_max_wait_ms value and
        # a very low eviction trigger threshold.
        self.conn.reconfigure('cache_max_wait_ms=2,cache_size=1MB')

        # Start a new transaction and insert a value far too large for cache.
        self.session.begin_transaction()
        cursor.set_key("key_b")
        cursor.set_value("a"*1024*5000)
        self.assertEqual(0, cursor.update())

        # Let WiredTiger's accounting catch up.
        time.sleep(2)

        # Perform cursor->next() which will result in the application thread being pulled into eviction
        # and getting rolled back. The auto retry is not expected to be performed, otherwise the test
        # will hang.
        got_rollback = False
        for i in range(80):
            try:
                read_cursor.next()
            except wiredtiger.WiredTigerError as e:
                if wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e):
                    got_rollback = True
                    break
                else:
                    raise e
        self.assertTrue(got_rollback, "Expected rollback to occur on cursor->next()")

        # A rollback should unposition the read cursor.
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, read_cursor.get_key, '/requires key be set/')

        self.ignoreStdoutPatternIfExists("Cache capacity has overflown")
