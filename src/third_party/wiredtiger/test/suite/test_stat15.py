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

import wttest
from wiredtiger import stat

# test_stat15.py
# Check that cache_pages_inuse and cache_pages_inuse_leaf are correctly tracked
class test_stat15(wttest.WiredTigerTestCase):
    uri = 'table:test_stat15'

    conn_config = 'statistics=(all),cache_size=100MB'

    def get_conn_stat(self, stat_key):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        val = stat_cursor[stat_key][2]
        stat_cursor.close()
        return val

    def test_cache_pages_inuse_leaf(self):
        # Create a table and insert enough data to bring leaf pages into cache
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1000):
            cursor[str(i).zfill(6)] = 'value_' + str(i)
        cursor.close()

        # Verify leaf pages are in cache
        leaf_pages = self.get_conn_stat(stat.conn.cache_pages_inuse_leaf)
        self.assertGreater(leaf_pages, 0,
            'expected cache_pages_inuse_leaf > 0 after inserting data')

        # Verify total pages >= leaf pages (total includes internal pages too)
        total_pages = self.get_conn_stat(stat.conn.cache_pages_inuse)
        self.assertGreaterEqual(total_pages, leaf_pages,
            'expected cache_pages_inuse >= cache_pages_inuse_leaf')

    def test_cache_pages_inuse_leaf_decreases_after_eviction(self):
        # Create a table and insert enough data to populate multiple leaf pages
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(10000):
            cursor[str(i).zfill(6)] = 'x' * 1000
        cursor.close()
        self.session.checkpoint(None)

        leaf_before = self.get_conn_stat(stat.conn.cache_pages_inuse_leaf)
        self.assertGreater(leaf_before, 0)

        # Force eviction by reopening the connection (clears cache).
        self.reopen_conn()

        leaf_after = self.get_conn_stat(stat.conn.cache_pages_inuse_leaf)
        self.assertLess(leaf_after, leaf_before,
            'expected cache_pages_inuse_leaf to decrease after cache cleared')
