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

from test_cc01 import test_cc_base
from suite_subprocess import suite_subprocess
from wiredtiger import stat
import os
import time
import wttest

# test_checkpoint33.py
#
# Test that checkpoint will not skip tables that have available space at the end that can be
# reclaimed through truncation.
class test_checkpoint33(test_cc_base, suite_subprocess):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    # conn_config = 'verbose=[checkpoint:2]'
    uri = 'table:test_checkpoint33'

    table_numkv = 1000000
    value_size = 1024
    value = 'a' * value_size
    min_file_size = 12 * 1024

    def delete(self, timestamp):
        c = self.session.open_cursor(self.uri, None)
        for k in range(self.table_numkv):
            c.set_key(k)
            self.session.begin_transaction()
            c.remove()
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(timestamp))
        c.close()

    def populate(self, timestamp):
        c = self.session.open_cursor(self.uri, None)
        for k in range(self.table_numkv):
            self.session.begin_transaction()
            c[k] = self.value
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(timestamp))
        c.close()

    def get_size(self):
        c = self.session.open_cursor('statistics:' + self.uri, None, None)
        file_size = c[stat.dsrc.block_size][2]
        c.close()
        return file_size

    def evict_all(self):
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        for k in range(self.table_numkv):
            evict_cursor.set_key(k)
            evict_cursor.search()
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

    # FIXME-WT-14982
    @wttest.skip_for_hook("disagg", "PALM environment mapsize limitation")
    def test_checkpoint33(self):

        if os.environ.get("TSAN_OPTIONS"):
            self.skipTest("FIXME-WT-14098 This test fails to compress the table when run under TSan")

        # Pin oldest timestamp 1.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Create and populate a table at timestamp 2.
        self.session.create(self.uri, self.create_params)
        self.session.checkpoint()
        self.populate(timestamp=2)

        # Make everything stable at timestamp 3.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(3)}')

        # Write to disk.
        self.session.checkpoint()

        # Delete everything at timestamp 4.
        self.delete(timestamp=4)

        # Make the deletions stable at timestamp 5.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(5)}')

        # Write to disk.
        self.session.checkpoint()

        # Evict all pages to ensure they all have disk blocks associated with them.
        self.evict_all()

        # Make everything globally visible.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(5)}')
        self.prout(f'File size: {self.get_size()}')

        # Wait for checkpoint cleanup to clean up all the deleted pages.
        self.wait_for_cc_to_run()

        # Checkpoint should recover the space by truncating the space made available by
        # checkpoint cleanup. Multiple checkpoints are required to move the blocks around and
        # eventually reach the minimum file size of 12KB.
        checkpoints = 0
        max_checkpoints = 10
        file_size = self.get_size()
        while file_size > self.min_file_size and checkpoints < max_checkpoints:
            self.session.checkpoint()
            file_size = self.get_size()
            checkpoints = checkpoints + 1
            self.prout(f'File size: {file_size}')
            self.prout(f'Checkpoints: {checkpoints}')

        self.assertLessEqual(self.get_size(), self.min_file_size)
