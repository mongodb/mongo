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

# test_compact07.py
# This test creates:
#
# - One table with the first 20% of keys deleted.
# - Two other tables with the first 90% of keys deleted.
#
# It checks that:
#
# - There is more available space in the last two tables.
# - The background compaction server only compacts the last two tables when the threshold set is
# above the available space of the first table.
# - Foreground compaction can be executed and can compact the first file with the lowest threshold. 
class test_compact07(wttest.WiredTigerTestCase):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(background_compact)'
    uri_prefix = 'table:test_compact07'
    
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
    
    def get_bg_compaction_files_tracked(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        files = stat_cursor[stat.conn.background_compact_files_tracked][2]
        stat_cursor.close()
        return files

    def get_files_compacted(self):
        files_compacted = 0
        for i in range(self.n_tables):
            uri = f'{self.uri_prefix}_{i}'
            if self.get_pages_rewritten(uri) > 0:
                files_compacted += 1
        return files_compacted

    def get_free_space(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        bytes = stat_cursor[stat.dsrc.block_reuse_bytes][2]
        stat_cursor.close()
        return bytes // megabyte
        
    def get_pages_rewritten(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, None)
        pages_rewritten = stat_cursor[stat.dsrc.btree_compact_pages_rewritten][2]
        stat_cursor.close()
        return pages_rewritten

    def get_size(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        size = stat_cursor[stat.dsrc.block_size][2]
        stat_cursor.close()
        return size

    def populate(self, uri, num_keys, value_size):
        c = self.session.open_cursor(uri, None)
        for k in range(num_keys):
            c[k] = ('%07d' % k) + '_' + 'abcd' * ((value_size // 4) - 2)
        c.close()
    
    # Test the basic functionality of the background compaction server. 
    def test_compact07(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")

        # Create and populate a table.
        uri_small = self.uri_prefix + '_small'
        self.session.create(uri_small, self.create_params)
        self.populate(uri_small, self.table_numkv, self.value_size)

        # Write to disk.
        self.session.checkpoint()

        # Delete the first 20% keys.
        self.delete_range(uri_small, 20 * self.table_numkv // 100)

        # Write to disk and retrieve the free space.
        self.session.checkpoint()
        free_space_20 = self.get_free_space(uri_small)

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

        # There should be more free space in the last created tables compared to the very first one.
        for i in range(self.n_tables):
            self.assertGreater(self.get_free_space(uri), free_space_20)

        # Enable background compaction with a threshold big enough so it does not process the first
        # table created but only the others with more empty space.
        self.session.compact(None, f'background=true,free_space_target={free_space_20 + 1}MB')
        
        # Wait for the background server to wake up.
        compact_running = self.get_bg_compaction_running()
        while not compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 1)
        
        # Background compaction should run through every file as listed in the metadata file.
        # Wait until all the eligible files have been compacted.
        while self.get_files_compacted() < self.n_tables:
            time.sleep(1)
            
        # Check that we made no progress on the small file.
        self.assertEqual(self.get_pages_rewritten(uri_small), 0)
        
        # Check the background compaction server stats. We should have skipped at least once and 
        # been successful at least once.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        skipped = stat_cursor[stat.conn.session_table_compact_skipped][2]
        success = stat_cursor[stat.conn.background_compact_success][2]
        self.assertGreater(skipped, 0)
        self.assertGreater(success, 0)
        stat_cursor.close()

        # Perform foreground compaction on the remaining file by setting a free_space_target value
        # that is guaranteed to run on it. This call might return EBUSY if background compaction is
        # inspecting it at the same time.
        self.compactUntilSuccess(self.session, uri_small, 'free_space_target=1MB')

        # Check that foreground compaction has done some work on the small table.
        self.assertGreater(self.get_pages_rewritten(uri_small), 0)

        # Check that we have some files in the background compaction tracking list.
        self.assertGreater(self.get_bg_compaction_files_tracked(), 0)

        # Drop the tables and wait for sometime for them to be removed from the background 
        # compaction server list.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            self.dropUntilSuccess(self.session, uri)
        
        self.session.checkpoint()
        
        # The tables should get removed from the tracking list once they exceed the max idle time 
        # after they're dropped. Only two tables are expected to be present: the small table and the
        # HS file.
        while self.get_bg_compaction_files_tracked() > 2:
            time.sleep(1)
        
        # Stop the background compaction server.
        self.session.compact(None, 'background=false')

        # Wait for the background compaction server to stop running.
        compact_running = self.get_bg_compaction_running()
        while compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 0)

        # Background compaction may be have been inspecting a table when disabled which is
        # considered as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')

if __name__ == '__main__':
    wttest.run()
