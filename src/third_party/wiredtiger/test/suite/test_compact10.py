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

import os, time
from compact_util import compact_util
from wiredtiger import stat
from wtbackup import backup_base

# test_compact10.py
# Verify compaction does not alter data by comparing full backups before/after compaction.
class test_compact10(backup_base, compact_util):

    conn_config = 'cache_size=100MB,statistics=(all)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB'
    uri_prefix = 'table:test_compact10'

    num_tables = 5
    table_numkv = 100 * 1000

    # This function generates the required data for the test by performing the following:
    # - Create N tables and populate them,
    # - Delete the first 50% of each table,
    # - Returns the URIs that were created.
    def generate_data(self):
        # Create and populate tables.
        uris = []
        for i in range(self.num_tables):
            uri = self.uri_prefix + f'_{i}'
            uris.append(uri)
            self.session.create(uri, self.create_params)
            compact_util.populate(self, uri, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Delete 50% of each file.
        for uri in uris:
            self.delete_range(uri, 50 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        return uris

    # This test:
    # - Creates a full backup before background compaction is enabled.
    # - Waits for background compaction to compact all the files and create a new full backup.
    # - Compares the two backups.
    def test_compact10(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction or backup")

        backup_1 = "BACKUP_1"
        backup_2 = "BACKUP_2"
        uris = self.generate_data()

        # Take the first full backup.
        os.mkdir(backup_1)
        self.take_full_backup(backup_1)

        # Enable background compaction. Only run compaction once to process each table and avoid
        # overwriting stats.
        self.turn_on_bg_compact('free_space_target=1MB,run_once=true')

        # Wait for background compaction to process all the tables.
        while self.get_bg_compaction_success() < self.num_tables:
            time.sleep(0.5)

        self.pr(f'Compaction has processed {self.get_bg_compaction_success()} tables.')
        self.assertTrue(self.get_bytes_recovered() > 0)

        # Take a second full backup.
        os.mkdir(backup_2)
        self.take_full_backup(backup_2)

        # Compare the backups.
        for uri in uris:
            self.compare_backups(uri, backup_1, backup_2)

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')
