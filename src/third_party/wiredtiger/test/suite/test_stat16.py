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

# test_stat16.py
# Verify cache_read_internal and cache_read_leaf statistics track page reads
# into cache separately by page type.
class test_stat16(wttest.WiredTigerTestCase):
    uri = 'table:test_stat16'

    # Small page sizes ensure multiple leaf pages and at least one internal page.
    conn_config = 'statistics=(all),cache_size=100MB'
    create_config = 'key_format=S,value_format=S,leaf_page_max=4KB,internal_page_max=4KB'

    def get_conn_stat(self, stat_key):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        val = stat_cursor[stat_key][2]
        stat_cursor.close()
        return val

    def test_cache_read_internal_and_leaf(self):
        self.session.create(self.uri, self.create_config)
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(5000):
            cursor[str(i).zfill(8)] = 'value_' + str(i)
        cursor.close()

        # Checkpoint so all pages are on disk.
        self.session.checkpoint(None)

        # Reopen to clear the cache, forcing subsequent reads to come from disk.
        self.reopen_conn()

        # Read all records to trigger cache misses for both leaf and internal pages.
        cursor = self.session.open_cursor(self.uri, None, None)
        while cursor.next() == 0:
            pass
        cursor.close()

        internal_reads = self.get_conn_stat(stat.conn.cache_read_internal)
        leaf_reads = self.get_conn_stat(stat.conn.cache_read_leaf)
        total_reads = self.get_conn_stat(stat.conn.cache_read)

        self.assertGreater(internal_reads, 0,
            'expected cache_read_internal > 0 after reading a multi-page btree')
        self.assertGreater(leaf_reads, 0,
            'expected cache_read_leaf > 0 after reading pages from disk')
        self.assertEqual(internal_reads + leaf_reads, total_reads,
            'cache_read_internal + cache_read_leaf must equal cache_read')
