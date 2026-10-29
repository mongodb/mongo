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
from error_info_util import error_info_util

# test_txn33.py
# A transaction that dirties more than the eviction updates trigger (or the dirty trigger, whichever
# is lower) allows can never bring the cache back under that trigger, because eviction cannot
# reclaim content pinned by an unresolved transaction. Such a transaction is doomed: it is
# conscripted into eviction on every operation and then cannot be released at commit, where rollback
# is no longer possible. Check that it is rolled back while it still can be.
class test_txn33(error_info_util):
    uri = "table:test_txn33"

    # A 50MB cache with the default 10% updates trigger puts the threshold at ~5MB. Keep the values
    # well under the default leaf_value_max so they are stored as ordinary items rather than
    # overflow items, and insert enough of them to cross the threshold.
    conn_config = "cache_size=50MB,statistics=(all)"
    value_size = 4 * 1024
    max_inserts = 4000

    # FIXME-WT-15058
    @wttest.skip_for_hook("disagg", "Fails due to incorrect cursor logic.")
    def test_txn_too_large_for_cache(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        cursor = self.session.open_cursor(self.uri)
        value = "a" * self.value_size

        # Accumulate dirty content in a single transaction until the rollback fires.
        self.session.begin_transaction()
        rolled_back = False
        for i in range(self.max_inserts):
            cursor.set_key(str(i))
            cursor.set_value(value)
            try:
                cursor.insert()
            except wiredtiger.WiredTigerError as e:
                self.assertTrue(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e), str(e))
                rolled_back = True
                break

        self.assertTrue(rolled_back,
            "transaction was not rolled back after {} inserts of {} bytes".format(
                self.max_inserts, self.value_size))

        self.assert_error_equal(wiredtiger.WT_ROLLBACK, wiredtiger.WT_TXN_TOO_LARGE_FOR_CACHE,
            "Transaction dirty content alone exceeds the eviction updates or dirty trigger")

        self.session.rollback_transaction()

        stat_cursor = self.session.open_cursor("statistics:", None, None)
        rollbacks = stat_cursor[wiredtiger.stat.conn.txn_rollback_too_large_for_cache][2]
        stat_cursor.close()
        self.assertGreater(rollbacks, 0)

        self.ignoreStdoutPatternIfExists(
            "Transaction dirty content alone exceeds the eviction updates or dirty trigger")

    def test_txn_within_cache_succeeds(self):
        """
        A transaction that stays under the updates trigger must not be affected.
        """
        self.session.create(self.uri, "key_format=S,value_format=S")
        cursor = self.session.open_cursor(self.uri)
        value = "a" * self.value_size

        # ~1MB of updates, comfortably under the ~5MB threshold.
        self.session.begin_transaction()
        for i in range(256):
            cursor[str(i)] = value
        self.session.commit_transaction()

        stat_cursor = self.session.open_cursor("statistics:", None, None)
        rollbacks = stat_cursor[wiredtiger.stat.conn.txn_rollback_too_large_for_cache][2]
        stat_cursor.close()
        self.assertEqual(rollbacks, 0)
