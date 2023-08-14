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
from wtdataset import SimpleDataSet
from wiredtiger import stat

megabyte = 1024 * 1024

# test_compact07.py
# Test background compaction server.
class test_compact07(wttest.WiredTigerTestCase):
    uri_prefix = 'table:test_compact07'
    conn_config = 'cache_size=100MB,statistics=(all)'
    key_format='i'
    value_format='S'
    
    delete_range_len = 100 * 1000
    delete_ranges_count = 4
    table_numkv = 500 * 1000
    n_tables = 5

    def get_bg_compaction_running(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        compact_running = stat_cursor[stat.conn.background_compact_running][2]
        stat_cursor.close()
        return compact_running

    def get_size(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        size = stat_cursor[stat.dsrc.block_size][2]
        stat_cursor.close()
        return size
    
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
        
    def delete_range(self, uri):
        c = self.session.open_cursor(uri, None)
        for r in range(self.delete_ranges_count):
            start = r * self.table_numkv // self.delete_ranges_count
            for i in range(self.delete_range_len):
                c.set_key(start + i)
                c.remove()
        c.close()

    def get_files_compacted(self):
        files_compacted = 0
        for i in range(self.n_tables):
            if (self.get_pages_rewritten(f'{self.uri_prefix}_{i}') > 0):
                files_compacted += 1
                
        return files_compacted
    
    # Test the basic functionality of the background compaction server. 
    def test_background_compact_usage(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")
        if self.runningHook('timestamp'):
            self.skipTest("timestamps are not relevant in this test")

        # Create a small table that compact should skip over.
        uri_small = self.uri_prefix + '_small'
        ds = SimpleDataSet(self, uri_small, self.table_numkv // 2, key_format=self.key_format, value_format=self.value_format)
        ds.populate()
        self.delete_range(uri_small)
              
        # Create n tables for background compaction to loop through.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            ds = SimpleDataSet(self, uri, self.table_numkv, 
                            key_format=self.key_format, 
                            value_format=self.value_format)
            ds.populate()
    
            # Now let's delete a lot of data ranges. Create enough space so that compact runs in more
            # than one iteration.
            self.delete_range(uri)

        # Reopen the connection to force the object to disk.
        self.reopen_conn()
        
        small_file_free_space = self.get_free_space(uri_small)
        
        # Allow background compaction to run for some time. Set a low free_space_target larger
        # than the smaller file.
        self.session.compact(None,f'background=true,free_space_target={small_file_free_space + 1}MB')
        
        # Wait for the background server to wake up.
        compact_running = self.get_bg_compaction_running()
        while not compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 1)
        
        # Background compaction should run through every file as listed in the metadata file.
        # Periodically check how many files we've compacted until we compact all of them.
        while self.get_files_compacted() < self.n_tables:
            time.sleep(1)
            
        # Check that we made no progress on the small file.
        self.assertEqual(self.get_pages_rewritten(uri_small), 0)
        
        # Check the background compaction server stats. We should have skipped at least once and 
        # been successful at least once.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        skipped = stat_cursor[stat.conn.background_compact_skipped][2]
        success = stat_cursor[stat.conn.background_compact_success][2]
        self.assertGreater(skipped, 0)
        self.assertGreater(success, 0)
        stat_cursor.close()
        
        # Open a new session for foreground compaction, while leaving the background
        # compaction server running.
        session2 = self.conn.open_session()
        # Use a free_space_target that is guaranteed to run on the small file.
        session2.compact(uri_small,f'free_space_target={small_file_free_space - 1}MB')

        # Check that foreground compaction has done some work on the small table.
        self.assertGreater(self.get_pages_rewritten(uri_small), 0)
                    
        # Disable the background compaction server.
        self.session.compact(None,'background=false')
        
        # Wait for the background server to stop running.
        compact_running = self.get_bg_compaction_running()
        while compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 0)
        
    # FIXME-WT-11450: It's possible for compaction to block the drop table call indefinitely.
    # We should reenable this test once that situation can be handled by the background compaction
    # server.
    
    # Test the background server while creating and dropping tables.
    # def test_background_compact_table_ops(self):
    #     # Trigger the background compaction server first, before creating tables.
    #     self.session.compact(None,'background=true,free_space_target=1MB')

    #     # Create n tables for background compaction to loop through.
    #     for i in range(self.n_tables):
    #         uri = self.uri_prefix + f'_{i}'
    #         ds = SimpleDataSet(self, uri, self.table_numkv, 
    #                         key_format=self.key_format, 
    #                         value_format=self.value_format)
    #         ds.populate()
                
    #     # Delete some data from half of the tables.
    #     for i in range(self.n_tables):
    #         if (i % 2 == 0):
    #             self.delete_range(f'{self.uri_prefix}_{i}')
                
    #     # Periodically check that background compaction has done some work on any of the tables.
    #     while (self.get_files_compacted() == 0):
    #         time.sleep(1)
            
    #     # Now drop all the tables.
    #     for i in range(self.n_tables):
    #         self.dropUntilSuccess(self.session, f'{self.uri_prefix}_{i}')
        
    # Run background compaction alongside many checkpoints.
    # FIXME-WT-11346: Revisit this test, it might be better to move it to the cppsuite.
    def test_background_compact_ckpt_stress(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")
        if self.runningHook('timestamp'):
            self.skipTest("timestamps are not relevant in this test")

        # Create n tables for background compaction to loop through.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            ds = SimpleDataSet(self, uri, self.table_numkv, 
                            key_format=self.key_format, 
                            value_format=self.value_format)
            ds.populate()
            self.delete_range(uri)
            
        self.reopen_conn()

        self.session.compact(None, 'background=true,free_space_target=1MB')    
        for i in range(1000):
            self.session.begin_transaction()
            cur = self.session.open_cursor(self.uri_prefix + '_0')
            for i in range(100):
                cur[i] = "aaaa"
            cur.close()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
            self.session.checkpoint()
        
if __name__ == '__main__':
    wttest.run()
