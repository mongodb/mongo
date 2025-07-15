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

import time, wttest
from compact_util import compact_util
from test_cc01 import test_cc_base
from wiredtiger import stat

kilobyte = 1024

# test_compact12.py
# This test creates:
#
# - One table with the first 1/4 of keys deleted and obsolete and the last 10th of the keys
#   removed with fast truncate but not yet obsolete.
#
# It checks that:
#
# - Compaction correctly rewrites pages in WT_REF_DELETED state but are still on disk.
class test_compact12(compact_util, test_cc_base):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,leaf_value_max=16MB'
    conn_config = 'cache_size=100MB,statistics=(all),verbose=[compact:4]'
    uri_prefix = 'table:test_compact12'

    table_numkv = 10 * 1000
    value_size = kilobyte # The value should be small enough so that we don't create overflow pages.

    def get_fast_truncated_pages(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        stat_cursor.close()
        return pages

    def populate(self, uri, num_keys):
        c = self.session.open_cursor(uri, None)
        for k in range(num_keys // 10 * 9):
            self.session.begin_transaction()
            c[k] = ('%07d' % k) + '_' + 'a' * self.value_size
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')

        for k in range(num_keys // 10 * 9, num_keys):
            self.session.begin_transaction()
            c[k] = ('%07d' % k) + '_' + 'b' * self.value_size
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')
        c.close()

    def test_compact12_truncate(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # FIXME-SLS-1890
        self.skipTest("This test is not robust to changes in eviction behavior")

        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Create and populate a table.
        uri = self.uri_prefix
        self.session.create(uri, self.create_params)

        self.populate(uri, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Remove 1/4 of the keys at timestamp 3.
        c = self.session.open_cursor(uri, None)
        for i in range(self.table_numkv // 4):
            self.session.begin_transaction()
            c.set_key(i)
            c.remove()
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(3)}')
        c.close()

        # Reopen connection to ensure everything is on disk.
        self.reopen_conn()

        # Move the oldest timestamp forward to make the previously removed keys obsolete.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(4)}')

        # Fast truncate the last tenth of a file at timestamp 5.
        self.session.begin_transaction()
        self.truncate(uri, self.table_numkv // 10 * 9, self.table_numkv)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        # Trigger checkpoint cleanup twice to remove the obsolete content.
        self.wait_for_cc_to_run()
        self.wait_for_cc_to_run()

        self.assertGreater(self.get_fast_truncated_pages(), 0)

        # Check the size of the table.
        size_before_compact = self.get_size(uri)

        self.session.compact(uri, 'free_space_target=1MB')

        # Ensure compact has moved the fast truncated pages at the end of the file.
        # We should have recovered at least 1/4 of the file.
        size_after_compact = self.get_size(uri)
        space_recovered = size_before_compact - size_after_compact
        self.assertGreater(space_recovered, size_before_compact // 4)

        # Ignore compact verbose messages used for debugging.
        self.ignoreStdoutPatternIfExists('WT_VERB_COMPACT')
        self.ignoreStderrPatternIfExists('WT_VERB_COMPACT')

if __name__ == '__main__':
    wttest.run()
