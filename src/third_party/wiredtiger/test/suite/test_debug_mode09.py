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
# Force update restore eviction, whenever we evict a page. The debug mode
# is only effective on high cache pressure as WiredTiger can potentially decide
# to do an update restore evict on a page, when the cache pressure requirements are not met.
# This means setting eviction target low and cache size high.
class test_debug_mode09(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=10MB,statistics=(all),eviction_target=10,debug_mode=(update_restore_evict=true)'
    uri = "table:test_debug_mode09"

    # Insert a bunch of data to trigger eviction
    def trigger_eviction(self, uri):
        cursor = self.session.open_cursor(uri)
        for i in range(0, 20000):
            self.session.begin_transaction()
            cursor[i] = 'b' * 500
            self.session.commit_transaction()

    def test_update_restore_evict(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')

        self.trigger_eviction(self.uri)

        # Read the statistics of pages that have been update restored without update_restore
        stat_cursor = self.session.open_cursor('statistics:')
        pages_update_restored = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()
        self.assertGreater(pages_update_restored, 0)
