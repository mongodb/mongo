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

# test_scrub_eviction_conditions.py
#
# Test to do the following steps.
# 1. Populate data with 10 records
# 2. Update a record to dirty state
# 3. Evict the dirty page
# 4. Check if the dirty page is scrub-evicted
class test_scrub_eviction_conditions(wttest.WiredTigerTestCase):
    def conn_config(self):
        # Small cache to force eviction, enable all statistics
        return 'cache_size=1MB,statistics=(all),eviction=(threads_max=4),eviction_target=10'

    def setUp(self):
        super().setUp()
        self.uri = 'table:test_scrub_eviction_conditions'
        self.session.create(self.uri, 'key_format=i,value_format=S')

    def populate_data(self, n=10):
        c = self.session.open_cursor(self.uri)
        for i in range(1, n + 1):
            c[i] = 'x' * 100
        c.close()

    def get_stat(self, stat_key):
        cursor = self.session.open_cursor("statistics:" + self.uri)
        value = cursor[stat_key][2]
        cursor.close()
        return value

    def test_page_eviction_is_scrubbed(self):
        self.populate_data()
        self.session.checkpoint()

        # Dirty a page
        c = self.session.open_cursor(self.uri)
        c[1] = 'dirty-data'
        c.close()

        # Evict the dirty page
        c = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        c.set_key(1)
        c.search()
        c.reset()
        c.close()

        # Record stats after eviction
        stat_cursor = self.session.open_cursor('statistics:')
        scrubbed_after = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()
        clean_after = self.get_stat(stat.dsrc.cache_eviction_clean)
        dirty_after = self.get_stat(stat.dsrc.cache_eviction_dirty)

        # Assert scrub eviction happened for dirty page
        # We expect a scrub-eviction because threshold for clean eviction should not have been reached
        self.assertGreater(scrubbed_after, 0,
            "Dirty page should be scrub-evicted.")

        self.assertEqual(clean_after, 0,
            "Clean eviction should not increase due to dirty page.")

        self.assertGreater(dirty_after, 0,
            "Expected dirty eviction to increase for the evicted dirty page.")

if __name__ == '__main__':
    wttest.run()
