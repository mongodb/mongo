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

# test_compact14.py
# This test checks that background compaction skips small files.
class test_compact14(compact_util):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    conn_config = 'cache_size=100MB,statistics=(all)'

    table_numkv = 1

    def test_compact14(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # Create an table and populate small amount of data.
        uri = "table:test_compact14"
        self.session.create(uri, self.create_params)
        self.populate(uri, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Enable background compaction.
        bg_compact_config = 'background=true,free_space_target=1MB'
        self.turn_on_bg_compact(bg_compact_config)

        while self.get_bg_compaction_files_skipped() == 0:
            time.sleep(0.1)
