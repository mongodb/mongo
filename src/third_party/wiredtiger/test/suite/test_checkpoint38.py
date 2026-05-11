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
from wtscenario import make_scenarios

# test_checkpoint38.py
#
# Insert ~1GB of data into a single table and checkpoint with parallel checkpoint threads enabled.
# Verify that the parallel checkpoint worker threads actually reconciled pages.
class test_checkpoint38(wttest.WiredTigerTestCase):

    thread_values = [
        ('threads_4', dict(checkpoint_threads=4)),
        ('threads_8', dict(checkpoint_threads=8)),
    ]

    scenarios = make_scenarios(thread_values)

    # Use a large cache to ensure all dirty pages remain in cache during checkpoint, even when
    # running with the tiered hook (which adds flush_tier overhead before the checkpoint walk).
    # Without a large enough cache, eviction pressure during insertion can remove dirty leaf pages
    # from cache before the checkpoint walk, leaving nothing for the parallel workers to reconcile.
    # Set eviction_checkpoint_target=0 to disable pre-checkpoint scrubbing as well.
    def conn_config(self):
        return ('cache_size=1GB,'
                'eviction_dirty_target=95,eviction_dirty_trigger=99,eviction_checkpoint_target=0'
                ',statistics=(all),statistics_log=(json,on_close,wait=1)'
                f',checkpoint_threads={self.checkpoint_threads}')

    def get_stat(self, stat_key):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat_key][2]
        stat_cursor.close()
        return val

    def test_parallel_checkpoint(self):
        uri = 'table:checkpoint38'
        value_size = 10000
        nrows = 55000
        value = 'a' * value_size

        self.session.create(uri, 'key_format=i,value_format=S,leaf_page_max=32KB')

        cursor = self.session.open_cursor(uri)
        for i in range(nrows):
            cursor[i] = value
        cursor.close()

        pages_reconciled_before = self.get_stat(stat.conn.checkpoint_pages_reconciled)
        parallel_pages_before = self.get_stat(stat.conn.checkpoint_parallel_pages_reconciled)

        self.session.checkpoint()

        pages_reconciled = self.get_stat(stat.conn.checkpoint_pages_reconciled) - pages_reconciled_before
        parallel_pages_reconciled = self.get_stat(stat.conn.checkpoint_parallel_pages_reconciled) - parallel_pages_before
        rec_pct = self.get_stat(stat.conn.checkpoint_sync_rec_pct)

        self.pr(f'Pages reconciled: {pages_reconciled}')
        self.pr(f'Pages reconciled by parallel checkpoint threads: {parallel_pages_reconciled}')
        self.pr(f'Percentage of checkpoint sync time spent in reconciliation: {rec_pct}%')

        self.assertGreater(pages_reconciled, 0)
        self.assertGreater(parallel_pages_reconciled, 0, 'Parallel checkpoint threads were expected to reconcile pages')
        self.assertGreater(rec_pct, 0, 'Percentage of checkpoint sync time spent in reconciliation was expected to be set')

if __name__ == '__main__':
    wttest.run()
