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
from wiredtiger import stat
from compact_util import compact_util

megabyte = 1024 * 1024

# test_compact13.py
# This test checks that background compaction resets statistics after being disabled.
class test_compact13(compact_util):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all)'
    uri_prefix = 'table:test_compact13'

    table_numkv = 100 * 1000
    n_tables = 2

    # Test background compaction stats are reset when after being disabled.
    def test_compact13(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # Create and populate tables.
        uris = []
        for i in range(self.n_tables):
            uri = f'{self.uri_prefix}_{i}'
            uris.append(uri)
            self.session.create(uri, self.create_params)
            self.populate(uri, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Enable background compaction.
        bg_compact_config = 'background=true,free_space_target=1MB'
        self.turn_on_bg_compact(bg_compact_config)

        # Nothing should be compacted.
        while self.get_bg_compaction_files_skipped() < self.n_tables + 1:
            time.sleep(0.1)

        self.turn_off_bg_compact()

        # Delete the first 90%.
        for i in range(self.n_tables):
            uri = self.uri_prefix + f'_{i}'
            self.delete_range(uri, 90 * self.table_numkv // 100)

        # Write to disk.
        self.session.checkpoint()

        # Now the files can be compacted.
        self.turn_on_bg_compact(bg_compact_config)

        while self.get_files_compacted(uris) < 2:
            time.sleep(0.1)
