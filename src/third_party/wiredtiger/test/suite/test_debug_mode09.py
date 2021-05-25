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

from wiredtiger import stat
import wttest

# test_debug_mode09.py
# Test the debug mode setting for update_restore_evict.
# Force update restore eviction, whenever we evict a page.
class test_debug_mode09(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(update_restore_evict=true)'

    def test_update_restore_evict(self):
        uri = "table:test_debug_mode09"
        self.session.create(uri, 'key_format=i,value_format=S')

        # Insert a bunch of content
        cursor = self.session.open_cursor(uri)
        for i in range(0, 100):
            self.session.begin_transaction()
            cursor[i] = 'a' * 500
            self.session.commit_transaction()
        cursor.close()

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        cursor = self.session.open_cursor(uri, None, "debug=(release_evict=true)")
        for i in range(0, 100):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0)
            cursor.reset()
        cursor.close()

        # Read the statistics of pages that have been update restored
        stat_cursor = self.session.open_cursor('statistics:')
        pages_update_restored = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()
        self.assertEqual(pages_update_restored, 1)
