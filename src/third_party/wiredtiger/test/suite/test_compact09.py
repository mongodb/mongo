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

import time
import wttest
from wiredtiger import stat

megabyte = 1024 * 1024

# test_compact09.py
# This test creates tables with the first 90% of keys deleted.
#
# It checks that background compaction only compacts a table when it is not part of the exclude
# list.
class test_compact09(wttest.WiredTigerTestCase):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(background_compact)'
    uri_prefix = 'table:test_compact09'

    table_numkv = 100 * 1000
    n_tables = 2
    value_size = 1024 # The value should be small enough so that we don't create overflow pages.

    def delete_range(self, uri, num_keys):
        c = self.session.open_cursor(uri, None)
        for i in range(num_keys):
            c.set_key(i)
            c.remove()
        c.close()

    def get_bg_compaction_running(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        compact_running = stat_cursor[stat.conn.background_compact_running][2]
        stat_cursor.close()
        return compact_running

    def get_bg_compaction_files_excluded(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        files = stat_cursor[stat.conn.background_compact_exclude][2]
        stat_cursor.close()
        return files

    def get_files_compacted(self):
        files_compacted = 0
        for i in range(self.n_tables):
            uri = f'{self.uri_prefix}_{i}'
            if self.get_pages_rewritten(uri) > 0:
                files_compacted += 1
        return files_compacted

    def get_pages_rewritten(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, None)
        pages_rewritten = stat_cursor[stat.dsrc.btree_compact_pages_rewritten][2]
        stat_cursor.close()
        return pages_rewritten

    def populate(self, uri, num_keys, value_size):
        c = self.session.open_cursor(uri, None)
        for k in range(num_keys):
            c[k] = ('%07d' % k) + '_' + 'abcd' * ((value_size // 4) - 2)
        c.close()

    def turn_off_bg_compact(self):
        self.session.compact(None, 'background=false')
        compact_running = self.get_bg_compaction_running()
        while compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 0)

    def turn_on_bg_compact(self, config):
        self.session.compact(None, config)
        compact_running = self.get_bg_compaction_running()
        while not compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 1)

    # Test the exclude list functionality of the background compaction server.
    def test_compact09(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")

        # Create and populate tables.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            self.session.create(uri, self.create_params)
            self.populate(uri, self.table_numkv, self.value_size)

        # Write to disk.
        self.session.checkpoint()

        # Delete the first 90%.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            self.delete_range(uri, 90 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        # Enable background compaction and exclude the two tables.
        exclude_list = f'["{self.uri_prefix}_0.wt", "{self.uri_prefix}_1.wt"]'
        config = f'background=true,free_space_target=1MB,exclude={exclude_list}'
        self.turn_on_bg_compact(config)

        # Background compaction should exclude all files.
        while self.get_bg_compaction_files_excluded() < self.n_tables:
            time.sleep(1)
        assert self.get_files_compacted() == 0
        num_files_excluded = self.get_bg_compaction_files_excluded()
        assert num_files_excluded == self.n_tables

        # Stop the background compaction server.
        self.turn_off_bg_compact()

        # Enable background compaction and exclude only one table.
        exclude_list = f'["{self.uri_prefix}_0.wt"]'
        config = f'background=true,free_space_target=1MB,exclude={exclude_list}'
        self.turn_on_bg_compact(config)

        # Background compaction should exclude only one file now. Since the stats are cumulative, we
        # need to take into account the previous check.
        while self.get_bg_compaction_files_excluded() < num_files_excluded + 1:
            time.sleep(1)
        assert self.get_bg_compaction_files_excluded() == num_files_excluded + 1

        # We should now start compacting the second table.
        while self.get_files_compacted() == 0:
            time.sleep(1)
        assert self.get_files_compacted() == 1

        # Make sure we have compacted the right table.
        uri = self.uri_prefix + '_0'
        self.assertEqual(self.get_pages_rewritten(uri), 0)
        uri = self.uri_prefix + '_1'
        self.assertGreater(self.get_pages_rewritten(uri), 0)

        # Stop the background compaction server.
        self.turn_off_bg_compact()

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')

if __name__ == '__main__':
    wttest.run()
