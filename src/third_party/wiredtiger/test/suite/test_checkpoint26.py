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

# test_checkpoint26.py
# Test the timing stress setting for checkpoint_evict_page.
# The setting forces checkpoint to evict all pages that are reconciled. The debug mode is effective 
# in testing scenarios where checkpoint itself starts to evict pages. Have a big enough cache and
# small data pages so that eviction activity is small, allowing checkpoint to reconcile and
# evict pages.
class test_checkpoint26(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=1000MB,statistics=(all),eviction_dirty_target=80,eviction_dirty_trigger=95,timing_stress_for_test=[checkpoint_evict_page]'
    uri = "table:test_checkpoint26"

    def test_checkpoint_evict_page(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(0, 10000):
            self.session.begin_transaction()
            cursor[i] = 'b' * 5000
            self.session.commit_transaction()

        # There should be no eviction activity at this point.
        stat_cursor = self.session.open_cursor('statistics:')
        pages_evicted_during_checkpoint = stat_cursor[stat.conn.cache_eviction_pages_in_parallel_with_checkpoint][2]
        self.assertEqual(pages_evicted_during_checkpoint, 0)
        stat_cursor.close()

        # Make checkpoint perform eviction.
        self.session.checkpoint()

        # Read the statistics of pages that have been evicted during checkpoint.
        stat_cursor = self.session.open_cursor('statistics:')
        pages_evicted_during_checkpoint = stat_cursor[stat.conn.cache_eviction_pages_in_parallel_with_checkpoint][2]
        self.assertGreater(pages_evicted_during_checkpoint, 0)
        stat_cursor.close()
