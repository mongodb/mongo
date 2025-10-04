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

# test_compact09.py
# This test creates tables with the first 90% of keys deleted.
#
# It checks that background compaction only compacts a table when it is not part of the exclude
# list.
class test_compact09(compact_util):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(background_compact)'
    uri_prefix = 'table:test_compact09'

    table_numkv = 100 * 1000
    n_tables = 2

    def get_bg_compaction_files_excluded(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        files = stat_cursor[stat.conn.background_compact_exclude][2]
        stat_cursor.close()
        return files

    # Test the exclude list functionality of the background compaction server.
    def test_compact09(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

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

        # Enable background compaction and exclude the two tables. Use run once to be able to
        # track the stats easily.
        exclude_list = f'["{self.uri_prefix}_0.wt", "{self.uri_prefix}_1.wt"]'
        config = f'background=true,free_space_target=1MB,exclude={exclude_list},run_once=true'
        # Don't use the helper function as the server may go to sleep before we have the time to
        # check it is actually running.
        self.session.compact(None, config)

        # Background compaction should exclude all files.
        while self.get_bg_compaction_files_excluded() < self.n_tables:
            time.sleep(0.1)
        assert self.get_files_compacted(uris) == 0
        num_files_excluded = self.get_bg_compaction_files_excluded()
        assert num_files_excluded == self.n_tables

        # Make sure the background server is stopped by now.
        while self.get_bg_compaction_running():
            time.sleep(0.1)

        # Enable background compaction and exclude only one table.
        exclude_list = f'["{self.uri_prefix}_0.wt"]'
        config = f'background=true,free_space_target=1MB,exclude={exclude_list},run_once=true'
        self.session.compact(None, config)

        # Background compaction should exclude only one file now. Since the stats are cumulative, we
        # need to take into account the previous check.
        while self.get_bg_compaction_files_excluded() < num_files_excluded + 1:
            time.sleep(0.1)
        assert self.get_bg_compaction_files_excluded() == num_files_excluded + 1

        # We should now start compacting the second table.
        while self.get_files_compacted(uris) == 0:
            time.sleep(0.1)
        assert self.get_files_compacted(uris) == 1

        # Make sure we have compacted the right table.
        uri = self.uri_prefix + '_0'
        self.assertEqual(self.get_pages_rewritten(uri), 0)
        uri = self.uri_prefix + '_1'
        self.assertGreater(self.get_pages_rewritten(uri), 0)

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')
