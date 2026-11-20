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

import os

import wttest
from wiredtiger import stat

# Check that a checkpoint with parallel reconciliation threads enabled reports the time the
# checkpoint thread spends walking trees and waiting for the workers, and that those times fit
# inside the per-tree checkpoint duration.
class test_checkpoint_parallel_stats(wttest.WiredTigerTestCase):

    # Keep dirty pages in cache so the checkpoint walk has work for the parallel workers.
    def conn_config(self):
        return ('cache_size=1GB,'
                'eviction_dirty_target=95,eviction_dirty_trigger=99,eviction_checkpoint_target=0'
                ',statistics=(all)'
                ',checkpoint_threads=4')

    def read_stats(self):
        return dict(
            pages=self.get_stat(stat.conn.checkpoint_parallel_pages_reconciled),
            walk=self.get_stat(stat.conn.checkpoint_walk_time),
            wait=self.get_stat(stat.conn.checkpoint_parallel_wait_time),
            tree=self.get_stat(stat.conn.checkpoint_tree_duration),
            pct=self.get_stat(stat.conn.checkpoint_sync_rec_pct))

    def test_checkpoint_parallel_stats(self):
        n_tables = 6
        nrows = 2000
        value = 'a' * 4000
        value2 = 'b' * 4000
        uris = ['table:parallel_stats{:02d}'.format(i) for i in range(n_tables)]

        for uri in uris:
            self.session.create(uri, 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(uri)
            for i in range(nrows):
                cursor[i] = value
            cursor.close()

        # The first checkpoint splits the pages the inserts grew. Dirty every tree again, so the
        # measured checkpoint walks trees of many pages.
        self.session.checkpoint()
        for uri in uris:
            cursor = self.session.open_cursor(uri)
            for i in range(nrows):
                cursor[i] = value2
            cursor.close()

        pages_before = self.get_stat(stat.conn.checkpoint_parallel_pages_reconciled)
        self.session.checkpoint()
        stats = self.read_stats()
        stats['pages'] -= pages_before
        self.pr('checkpoint stats: {}'.format(stats))

        self.assertGreater(stats['pages'], 0,
            'Parallel checkpoint workers were expected to reconcile pages')

        # WiredTiger reads the clock from the system time on Windows, which only advances once per
        # timer tick, so intervals this short read as zero. Checking the timers there would need a
        # tree large enough for the walk to span several ticks.
        if os.name != 'nt':
            self.assertGreater(stats['walk'], 0,
                'The checkpoint thread was expected to spend time walking trees')
            self.assertGreater(stats['wait'], 0,
                'The checkpoint thread was expected to wait for parallel workers')
            self.assertGreater(stats['tree'], 0,
                'Per-tree checkpoint duration was expected to be set')

            # Both times are intervals inside the per-tree wall time. The metadata tree,
            # checkpointed outside this window, adds a few pages to the two timers, far fewer than
            # the remainder they do not account for.
            self.assertLessEqual(stats['walk'] + stats['wait'], stats['tree'],
                'Walk and worker wait should fit inside the per-tree duration')

if __name__ == '__main__':
    wttest.run()
