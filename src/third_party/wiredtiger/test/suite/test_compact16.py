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

import threading, time
from compact_util import compact_util
from wiredtiger import stat
from wtthread import checkpoint_thread

# test_compact16
# Test compact and checkpoint running concurrently, ensuring space is reclaimed after the compaction
# call.
class test_compact16(compact_util):
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,leaf_value_max=16MB'
    conn_config = 'cache_size=100MB,statistics=(all),verbose=[compact:2]'
    uri = 'table:test_compact16'

    table_numkv = 1000 * 1000

    def test_compact16(self):
        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # Create and populate a table.
        self.session.create(self.uri, self.create_params)
        self.populate(self.uri, 0, self.table_numkv)

        # Write to disk.
        self.session.checkpoint()

        # Remove 1/4 of the data.
        self.delete_range(self.uri, self.table_numkv // 4)

        # Write everything to disk.
        self.reopen_conn()

        # Run compact concurrently with another thread that continually creates checkpoints.
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start before calling compact.
            ckpt_started = False
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.checkpoint_state][2] != 0
                stat_cursor.close()
                time.sleep(0.1)
            self.session.compact(self.uri)
        finally:
            done.set()
            ckpt.join()

        # Compact should not leave more than 20% available space in the file. If it does, that means
        # we've hit the maximum (100) passes over a file for a compaction attempt and something else
        # has prevented compact from reclaiming space.
        pct_space_available = self.get_bytes_avail_for_reuse(self.uri) / self.get_size(self.uri) * 100
        self.assertLess(pct_space_available, 20)

        self.ignoreStdoutPatternIfExists('WT_VERB_COMPACT')
        self.ignoreStderrPatternIfExists('WT_VERB_COMPACT')
