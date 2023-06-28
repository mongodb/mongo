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

import os
from unittest import skip
import wiredtiger, wttest

# test_stat08.py
#    Session statistics for bytes read into the cache.
class test_stat08(wttest.WiredTigerTestCase):

    nentries = 100000
    # Leave the cache size on the default setting to avoid filling up the cache
    # too much and triggering unnecessary rollbacks. But make the value fairly
    # large to make obvious change to the statistics.
    conn_config = 'statistics=(all)'
    entry_value = "abcde" * 400
    BYTES_READ = wiredtiger.stat.session.bytes_read
    READ_TIME = wiredtiger.stat.session.read_time
    session_stats = { BYTES_READ : "session: bytes read into cache",           \
        READ_TIME : "session: page read from disk to cache time (usecs)"}

    def get_stat(self, stat):
        statc =  self.session.open_cursor('statistics:session', None, None)
        val = statc[stat][2]
        statc.close()
        return val

    def get_cstat(self, stat):
        statc =  self.session.open_cursor('statistics:', None, None)
        val = statc[stat][2]
        statc.close()
        return val

    def check_stats(self, cur, k):
        #
        # Some Windows machines lack the time granularity to detect microseconds.
        # Skip the time check on Windows.
        #
        if os.name == "nt" and k is self.READ_TIME:
            return

        exp_desc = self.session_stats[k]
        cur.set_key(k)
        cur.search()
        [desc, pvalue, value] = cur.get_values()
        self.pr('  stat: \'%s\', \'%s\', \'%s\'' % (desc, pvalue, str(value)))
        self.assertEqual(desc, exp_desc)
        if k is self.BYTES_READ or k is self.READ_TIME:
            self.assertTrue(value > 0)

    def test_session_stats(self):
        # We want to configure for pages to be explicitly evicted when we are done with them so
        # that we can correctly verify the statistic measuring bytes read from cache.
        self.session = self.conn.open_session("debug=(release_evict_page=true)")
        self.session.create("table:test_stat08", "key_format=i,value_format=S")
        cursor =  self.session.open_cursor('table:test_stat08', None, None)
        self.session.begin_transaction()
        txn_dirty = self.get_stat(wiredtiger.stat.session.txn_bytes_dirty)
        cache_dirty = self.get_cstat(wiredtiger.stat.conn.cache_bytes_dirty)
        self.assertEqual(txn_dirty, 0)
        self.assertLessEqual(txn_dirty, cache_dirty)
        # Write the entries.
        for i in range(1, self.nentries):
            txn_dirty_before = self.get_stat(wiredtiger.stat.session.txn_bytes_dirty)
            cursor[i] = self.entry_value
            txn_dirty_after = self.get_stat(wiredtiger.stat.session.txn_bytes_dirty)
            self.assertLess(txn_dirty_before, txn_dirty_after)
            # Since we're using an explicit transaction, we need to resolve somewhat frequently.
            # So check the statistics and restart the transaction every 200 operations.
            if i % 200 == 0:
                cache_dirty_txn = self.get_cstat(wiredtiger.stat.conn.cache_bytes_dirty)
                # Make sure the txn's dirty bytes doesn't exceed the cache.
                self.assertLessEqual(txn_dirty_after, cache_dirty_txn)
                self.session.rollback_transaction()
                self.session.begin_transaction()
                txn_dirty = self.get_stat(wiredtiger.stat.session.txn_bytes_dirty)
                self.assertEqual(txn_dirty, 0)
        self.session.commit_transaction()
        cursor.reset()

        # Read the entries.
        i = 0
        for key, value in cursor:
            i = i + 1
        cursor.reset()

        # Now check the session statistics for bytes read into the cache.
        stat_cur = self.session.open_cursor('statistics:session', None, None)
        for k in self.session_stats:
            self.check_stats(stat_cur, k)

        # Session stats cursor reset should set all the stats values to zero.
        stat_cur.reset()
        while stat_cur.next() == 0:
            [desc, pvalue, value] = stat_cur.get_values()
            self.assertTrue(value == 0)

if __name__ == '__main__':
    wttest.run()
