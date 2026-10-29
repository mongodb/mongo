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
from wiredtiger import stat

# test_txn34.py
#   The begin_transaction "ignore_cache_size" setting exempts a transaction from being pulled
#   into eviction, and stops applying once the transaction ends.
class test_txn34(wttest.WiredTigerTestCase):
    uri = 'table:test_txn34.wt'

    def fill_cache(self):
        """Leave the cache over its limits, with a reader session positioned in the table."""
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(100):
            cursor.set_key('key_a' + str(i))
            cursor.set_value('a' * 1024)
            self.assertEqual(0, cursor.insert())
        self.session.commit_transaction()

        evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        for i in range(100):
            evict_cursor.set_key('key_a' + str(i))
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()

        reader = self.conn.open_session()
        read_cursor = reader.open_cursor(self.uri)
        read_cursor.set_key('key_a10')
        read_cursor.search_near()

        self.conn.reconfigure('cache_max_wait_ms=2,cache_size=1MB')

        self.session.begin_transaction()
        cursor.set_key('key_b')
        cursor.set_value('a' * 1024 * 5000)
        self.assertEqual(0, cursor.update())

        self.wait_for_cache_over_limit()

        return reader, read_cursor

    def wait_for_cache_over_limit(self):
        """Wait until the cache is reported as being over the configured limit."""
        deadline = time.time() + 60
        while True:
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            in_use = stat_cursor[stat.conn.cache_bytes_inuse][2]
            limit = stat_cursor[stat.conn.cache_bytes_max][2]
            stat_cursor.close()
            if in_use > limit:
                return
            self.assertLess(time.time(), deadline,
                'timed out waiting for the cache to fill: %d of %d bytes in use' % (in_use, limit))
            time.sleep(0.1)

    # Traversals to attempt before concluding that the reader is never rolled back. The table holds
    # far fewer records than this, so a reader that obeys the cache size has many opportunities to
    # be pulled into eviction.
    read_attempts = 500

    def rolled_back_while_reading(self, reader, read_cursor, txn_config, resolve='rollback'):
        """Read until rolled back, then resolve the transaction the requested way."""
        reader.begin_transaction(txn_config)
        rolled_back = False
        try:
            for _ in range(self.read_attempts):
                try:
                    read_cursor.next()
                except wiredtiger.WiredTigerError as e:
                    if wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e):
                        rolled_back = True
                        break
                    raise e
            return rolled_back
        finally:
            # A rolled-back transaction can only be rolled back, so the caller's choice of how to
            # resolve it only applies when the read succeeded.
            if rolled_back or resolve == 'rollback':
                reader.rollback_transaction()
            else:
                reader.commit_transaction()
            read_cursor.reset()

    @wttest.skip_for_hook("disagg", "disagg requires an additional condition to evict pages")
    def test_ignore_cache_size(self):
        reader, read_cursor = self.fill_cache()

        # A transaction that obeys the cache size is pulled into eviction and rolled back.
        self.assertTrue(self.rolled_back_while_reading(reader, read_cursor, None))

        self.ignoreStdoutPatternIfExists('Cache capacity has overflown')

        # The same work under "ignore_cache_size" is never blocked by the full cache.
        self.assertFalse(
            self.rolled_back_while_reading(reader, read_cursor, 'ignore_cache_size=true'))

        # The setting does not outlive the transaction that requested it, whether that transaction
        # was rolled back or committed.
        self.assertTrue(self.rolled_back_while_reading(reader, read_cursor, None))
        self.assertFalse(self.rolled_back_while_reading(
            reader, read_cursor, 'ignore_cache_size=true', resolve='commit'))
        self.assertTrue(self.rolled_back_while_reading(reader, read_cursor, None))

        self.ignoreStdoutPatternIfExists('Cache capacity has overflown')
