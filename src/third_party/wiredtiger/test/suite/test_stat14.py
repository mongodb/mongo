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
from wtscenario import make_scenarios
from helper import WiredTigerStat, WiredTigerCursor

# test_stat14.py
# This test verifies the block-space reusability indicators at both the file and connection levels
# when reusable space exceeds 50% and 90%. The test relies on WiredTigers sequential block
# allocation: newer inserts land at the end of the file, while deleting earlier keys creates holes
# in earlier blocks and increases the bytes available for reuse. Reusability indicators are
# evaluated only when the file size is at least 100MB (hardcoded threshold). Therefore, the test
# exercises two scenarios:
#   Small file (<100MB): the threshold-based reusability stats should remain zero.
#   Large file (100MB): the indicators should transition as reusable space crosses 50% and 90%.
# To reach the threshold quickly, the test writes large value payloads. This test is not applicable
# to disaggregated storage, which does not model contiguous on-disk block layout; block-space
# reusability percentages are not meaningful in that environment

@wttest.skip_for_hook("disagg", "Block size only works for ASC mode")
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_stat14(wttest.WiredTigerTestCase):
    uri = 'table:test_stat14'

    conn_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true)'
    # Try to occupy more pages for given keys
    long_v = 's'*1000

    table_cnt = 4

    table_scale = [
        ('small_table', dict(is_small = True, key_cnt = int(1.3e3))),
        ('large_table', dict(is_small = False, key_cnt = int(1.3e5))),
    ]
    scenarios = make_scenarios(table_scale)

    def populate(self, uri):
        with WiredTigerCursor(self.session, uri, None, None) as cursor:
            for i in range(self.key_cnt):
                cursor[i] = self.long_v
        # Call eviction to the keys to make sure the changes write to the disk.
        with WiredTigerCursor(self.session, uri, None, "debug=(release_evict)") as cursor:
            for i in range(self.key_cnt):
                cursor.set_key(i)
                cursor.search()
        self.session.checkpoint()

    def stat_check(self, uri, reuse_50, reuse_90):
        with WiredTigerStat(self.session, uri) as stat_cursor:
            reusable_size = stat_cursor[stat.dsrc.block_reuse_bytes][2]
            block_size = stat_cursor[stat.dsrc.block_size][2]
            if not self.is_small:
                self.assertEqual(reuse_50, 1 if reusable_size >= 0.5*block_size else 0)
                self.assertEqual(reuse_90, 1 if reusable_size >= 0.9*block_size else 0)

    def evict_between(self, uri, start, end):
        # Call eviction to the keys to make sure the changes write to the disk.
        with WiredTigerCursor(self.session, uri, None, "debug=(release_evict)") as cursor:
            for i in range(start, end):
                cursor.set_key(i)
                cursor.search()

    def clear_between(self, uri, start, end):
        with WiredTigerCursor(self.session, uri, None, None) as cursor:
            for i in range(start, end):
                cursor.set_key(i)
                cursor.remove()

    def clean(self, uri, skip_check = False):
        split_60 = int(self.key_cnt*0.6)
        split_99 = int(self.key_cnt*0.99)
        # Remove the first 60% of the file, verify we have at least 50% free.
        self.clear_between(uri, 0, split_60)
        self.session.checkpoint()
        # We need second checkpoint call to sync the disk status.
        self.evict_between(uri, 0, split_60)
        self.session.checkpoint()
        if not skip_check:
            self.stat_check(uri, 1, 0)
        # Remove more and check we have 90% of the file free.
        self.clear_between(uri, split_60, split_99)
        self.session.checkpoint()
        self.evict_between(uri, split_60, split_99)
        self.session.checkpoint()
        if not skip_check:
            self.stat_check(uri, 1, 1)

    def table_name(self, i:int):
        return f'{self.uri}_{i}'

    def test_reusable_percentage(self):
        # Populate a table with a few records. This will create a two-level tree with a root
        # page and one or more leaf pages. We aren't inserting nearly enough records to need
        # an additional level
        create_params = 'key_format=i,value_format=S'
        for i in range(self.table_cnt):
            self.session.create(self.table_name(i), create_params)
            self.populate(self.table_name(i))
        # The reason to always check target number and target number + 1 is that the
        # we cannot guarantee the history store will meet the ratio at the same time or not.
        for i in range(self.table_cnt):
            with WiredTigerStat(self.session) as stat_cursor:
                files_over_50 = stat_cursor[stat.conn.block_reusable_over_50][2]
                files_over_90 = stat_cursor[stat.conn.block_reusable_over_90][2]
                self.assertIn(files_over_50, [0] if self.is_small else [i, i+1])
                self.assertIn(files_over_90, [0] if self.is_small else [i, i+1])
            # Skip the uri stat fetch to test trigger by checkpoint.
            self.clean(self.table_name(i), i > 0)
        with WiredTigerStat(self.session) as stat_cursor:
            files_over_50 = stat_cursor[stat.conn.block_reusable_over_50][2]
            files_over_90 = stat_cursor[stat.conn.block_reusable_over_90][2]
            self.assertIn(files_over_50, [0] if self.is_small else [self.table_cnt, self.table_cnt + 1])
            self.assertIn(files_over_90, [0] if self.is_small else [self.table_cnt, self.table_cnt + 1])
        for i in range(2):
            self.session.drop(self.table_name(i), None)
        self.session.checkpoint()
        with WiredTigerStat(self.session) as stat_cursor:
            files_over_50 = stat_cursor[stat.conn.block_reusable_over_50][2]
            files_over_90 = stat_cursor[stat.conn.block_reusable_over_90][2]
            self.assertIn(files_over_50, [0] if self.is_small else [self.table_cnt - 2, self.table_cnt - 1])
            self.assertIn(files_over_90, [0] if self.is_small else [self.table_cnt - 2, self.table_cnt - 1])
