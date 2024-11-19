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

import os, shutil, re
from wtbackup import backup_base
from compact_util import compact_util

# test_compact11.py
# Verify background compaction and incremental backup behaviour. The block modifications bits in an
# incremental backup should never be cleared when background compact is working on tables.
class test_compact11(backup_base, compact_util):
    backup_incr = "BACKUP_INCR"
    backup_full = "BACKUP_FULL"
    conn_config = 'cache_size=100MB,statistics=(all)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB'
    uri_prefix = 'table:test_compact11'

    num_tables = 5
    table_numkv = 100 * 1000

    def parse_blkmods(self, uri):
        meta_cursor = self.session.open_cursor('metadata:')
        config = meta_cursor[uri]
        meta_cursor.close()
        # The search string looks like: ,blocks=feffff1f000000000000000000000000
        # Obtain just the hex string.
        b = re.search(',blocks=(\w+)', config)
        self.assertTrue(b)
        # The bitmap string after the = is in group 1.
        blocks = b.group(1)
        self.pr(f"parse_blkmods for '{uri}': {blocks}")
        return blocks

    def test_compact11(self):
        # Create N tables.
        uris = []
        files = []
        bitmaps = []
        for i in range(self.num_tables):
            uri = self.uri_prefix + f'_{i}'
            uris.append(uri)
            files.append(f'file:test_compact11_{i}.wt')
            self.session.create(uri, self.create_params)

        # Populate the first half of each table.
        for uri in uris:
            compact_util.populate(self, uri, 0, self.table_numkv // 2)

        # Write to disk.
        self.session.checkpoint()

        # Take a full backup that will be used for incremental backups later during the test.
        os.mkdir(self.backup_incr)
        self.initial_backup = True
        self.take_full_backup(self.backup_incr)
        shutil.copytree(self.backup_incr, self.home_tmp)
        self.initial_backup = False

        # Insert the latter 50% in each table.
        for uri in uris:
            compact_util.populate(self, uri, self.table_numkv // 2, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Delete 50% of each file.
        for uri in uris:
            self.delete_range(uri, 50 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        # Take a full backup, this one will remain untouched.
        os.mkdir(self.backup_full)
        self.take_full_backup(self.backup_full)

        # Get the bitmaps of each file.
        for uri in files:
            bitmaps.append(self.parse_blkmods(uri))

        # Turn on background compaction to allow the bitmap blocks to be modified from compact
        # operation. Only run compaction once to process each table and avoid overwriting stats.
        self.turn_on_bg_compact('free_space_target=1MB,run_once=true')

        bytes_recovered = 0
        # Wait for background compaction to process all the tables.
        while self.get_bg_compaction_success() < self.num_tables:
            new_bytes_recovered = self.get_bytes_recovered()
            if new_bytes_recovered != bytes_recovered:
                # Update the incremental backup ID from the parent class.
                self.bkup_id += 1
                shutil.copytree(self.home_tmp, self.backup_incr + str(self.bkup_id))
                self.take_incr_backup(self.backup_incr + str(self.bkup_id), 0, self.bkup_id)

                bytes_recovered = new_bytes_recovered

        self.pr(f'Compaction has processed {self.get_bg_compaction_success()} tables.')
        self.assertTrue(bytes_recovered > 0)

        # Compare all the incremental backups against the starting full backup. The idea is that
        # compact should not have changed the contents of the table.
        for i in range(1, self.bkup_id + 1):
            for uri in uris:
                self.compare_backups(uri, self.backup_incr + str(i), self.backup_full)

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')
