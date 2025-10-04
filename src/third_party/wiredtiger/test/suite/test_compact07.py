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
from compact_util import compact_util
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
class test_compact07(compact_util):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(background_compact)'
    uri_prefix = 'table:test_compact07'

    table_numkv = 100 * 1000
    n_tables = 2

    def get_bg_compaction_files_tracked(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        files = stat_cursor[stat.conn.background_compact_files_tracked][2]
        stat_cursor.close()
        return files

    def get_free_space(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        bytes = stat_cursor[stat.dsrc.block_reuse_bytes][2]
        stat_cursor.close()
        return bytes // megabyte

    # Test the basic functionality of the background compaction server.
    def test_compact07(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # Create and populate a table.
        uri_small = self.uri_prefix + '_small'
        self.session.create(uri_small, self.create_params)
        self.populate(uri_small, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Delete the first 20% keys.
        self.delete_range(uri_small, 20 * self.table_numkv // 100)

        # Write to disk and retrieve the free space.
        self.session.checkpoint()
        free_space_20 = self.get_free_space(uri_small)

        # Create and populate tables.
        uris = []
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            uris.append(uri)
            self.session.create(uri, self.create_params)
            self.populate(uri, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Delete the first 90% of each file.
        for uri in uris:
            self.delete_range(uri, 90 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        # There should be more free space in the last created tables compared to the very first one.
        for i in range(self.n_tables):
            self.assertGreater(self.get_free_space(uri), free_space_20)

        # Enable background compaction with a threshold big enough so it does not process the first
        # table created but only the others with more empty space. We set run_once so we don't
        # conflict with foreground compaction. If the two are running in parallel, stats cannot be
        # checked properly. Don't use the helper function as the server may go to sleep before we
        # have the time to check it is actually running.
        bg_compact_config = f'background=true,free_space_target={(free_space_20 + 1)}MB,run_once=true'
        self.session.compact(None, bg_compact_config)

        # Background compaction should run through every file as listed in the metadata file.
        # Wait until all the eligible files have been compacted.
        while self.get_files_compacted(uris) < self.n_tables:
            time.sleep(0.1)

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
        # that is guaranteed to run on it. Make sure background compaction is disabled before
        # proceeding.
        while self.get_bg_compaction_running():
            time.sleep(0.1)
        self.compactUntilSuccess(self.session, uri_small, 'free_space_target=1MB')

        # Check that foreground compaction has done some work on the small table.
        self.assertGreater(self.get_pages_rewritten(uri_small), 0)

        # Restart background compaction and wait for it to start tracking files.
        # We expect to track each file.
        self.turn_on_bg_compact(f'free_space_target={(free_space_20 + 1)}MB')
        while self.get_bg_compaction_files_tracked() != self.n_tables + 1:
            time.sleep(0.1)

        # Drop the tables.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            self.dropUntilSuccess(self.session, uri)

        self.session.checkpoint()

        # The tables should get removed from the tracking list once they exceed the max idle time
        # after they're dropped.
        while self.get_bg_compaction_files_tracked() > 1:
            time.sleep(1)

        # Stop the background compaction server.
        self.turn_off_bg_compact()

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')
        self.ignoreStderrPatternIfExists('Compaction already happening')
