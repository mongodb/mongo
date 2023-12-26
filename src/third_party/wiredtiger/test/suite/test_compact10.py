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

import os, shutil, time, wttest
from wiredtiger import stat
from wtbackup import backup_base

# test_compact10.py
# Verify compaction does not alter data by comparing backups before/after compaction.
class test_compact10(backup_base):

    conn_config = 'cache_size=100MB,statistics=(all)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB'
    uri_prefix = 'table:test_compact10'

    num_tables = 5
    table_numkv = 100 * 1000
    value_size = 1024 # The value should be small enough so that we don't create overflow pages.

    def delete_range(self, uri, num_keys):
        c = self.session.open_cursor(uri, None)
        for i in range(num_keys):
            c.set_key(i)
            c.remove()
        c.close()

    def get_bg_compaction_running(self):
        return self.get_stat(stat.conn.background_compact_running)

    def get_bytes_recovered(self):
        return self.get_stat(stat.conn.background_compact_bytes_recovered)

    def get_files_compacted(self, uris):
        files_compacted = 0
        for uri in uris:
            if self.get_pages_rewritten(uri) > 0:
                files_compacted += 1
        return files_compacted

    def get_pages_rewritten(self, uri):
        return self.get_stat(stat.dsrc.btree_compact_pages_rewritten, uri)

    def get_stat(self, stat, uri = None):
        if not uri:
            uri = ''
        stat_cursor = self.session.open_cursor(f'statistics:{uri}', None, None)
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

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
            self.populate(uri, self.table_numkv, self.value_size)

        # Write to disk.
        self.session.checkpoint()

        # Delete 50% of the file.
        for uri in uris:
            self.delete_range(uri, 50 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        return uris

    def populate(self, uri, num_keys, value_size):
        c = self.session.open_cursor(uri, None)
        for k in range(num_keys):
            c[k] = ('%07d' % k) + '_' + 'abcd' * ((value_size // 4) - 2)
        c.close()

    def turn_on_bg_compact(self, config):
        self.session.compact(None, config)
        compact_running = self.get_bg_compaction_running()
        while not compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 1)

    # This test:
    # - Creates a full backup before background compaction is enabled.
    # - Waits for background compaction to compact all the files and create a new full backup.
    # - Compares the two backups.
    def test_compact10_full_backup(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")

        backup_1 = "BACKUP_1"
        backup_2 = "BACKUP_2"
        uris = self.generate_data()

        # Take the first full backup.
        os.mkdir(backup_1)
        self.take_full_backup(backup_1)

        # Enable background compaction.
        compact_config = f'background=true,free_space_target=1MB'
        self.turn_on_bg_compact(compact_config)

        # Wait for all tables to be compacted but the HS.
        while self.get_files_compacted(uris) < self.num_tables:
            time.sleep(0.5)

        assert self.get_files_compacted(uris) == self.num_tables

        # Take a second full backup.
        os.mkdir(backup_2)
        self.take_full_backup(backup_2)

        # Compare the backups.
        for uri in uris:
            self.compare_backups(uri, backup_1, backup_2)

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')

    # This test:
    # - Creates two full backups before background compaction is enabled, one that will be used for
    # comparison and another one as a base for incremental backups.
    # - Performs incremental backups as compaction is rewriting the files until all files have been
    # compacted.
    # - Compares all performed incremental backups with the original full backup.
    def test_compact10_incr_backup(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("this test does not yet work with tiered storage")

        backup_full = "BACKUP_FULL"
        backup_incr = "BACKUP_INCR"
        uris = self.generate_data()

        # Take two full backups:
        # - The first one will remain untouched.
        os.mkdir(backup_full)
        self.take_full_backup(backup_full)

        # - The second one is used for the incremental backups and use a temporary backup
        # directory to perform multiple incremental backups on different directories.
        self.initial_backup = True
        os.mkdir(backup_incr)
        self.take_full_backup(backup_incr)
        shutil.copytree(backup_incr, self.home_tmp)
        self.initial_backup = False

        # Enable background compaction.
        compact_config = f'background=true,free_space_target=1MB'
        self.turn_on_bg_compact(compact_config)

        # Every time compaction makes progress, perform an incremental backup.
        bytes_recovered = 0
        while self.get_files_compacted(uris) < self.num_tables:
            new_bytes_recovered = self.get_bytes_recovered()
            if new_bytes_recovered != bytes_recovered:
                # Update the incremental backup ID from the parent class.
                self.bkup_id += 1
                shutil.copytree(self.home_tmp, backup_incr + str(self.bkup_id))
                self.take_incr_backup(backup_incr + str(self.bkup_id))
                bytes_recovered = new_bytes_recovered

        # Compare all the incremental backups against the starting full backup. The idea is that
        # compact should not have changed the contents of the table.
        for i in range(1, self.bkup_id + 1):
            for uri in uris:
                self.compare_backups(uri, backup_incr + str(i), backup_full)

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')

if __name__ == '__main__':
    wttest.run()
