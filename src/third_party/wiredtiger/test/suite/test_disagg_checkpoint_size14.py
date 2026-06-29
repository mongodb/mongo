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

import wttest
from wiredtiger import stat
from helper_disagg import DisaggSizeTestMixin, disagg_test_class

# test_disagg_checkpoint_size14.py
#   Targets the failed-installation retry path: a reconciliation produces a
#   multi-block result, the multi-block split path returns EBUSY (the failpoint
#   or real contention), and the page stays in multi block. The next dirty
#   cycle then re-reconciles with fresh reconciliation multi-block state -- and
#   the OLD state's contribution to the running total must be subtracted before
#   the new state is installed. If it isn't, the running total drifts and
#   either trips the running-total decrement assertion or inflates the recorded
#   ckpt.size unbounded.

@disagg_test_class
class test_disagg_checkpoint_size14(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size14'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=20,leaf_page_delta=true,max_consecutive_delta=10),'
        'cache_size=4MB,'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base
    stable_uri = 'file:' + uri_base + '.wt_stable'
    table_config = 'key_format=S,value_format=S,leaf_page_max=8KB,internal_page_max=8KB'

    def insert_rows(self, cursor, start, count, value_char):
        value = value_char * 1024
        for i in range(start, start + count):
            cursor[f'key{i:08d}'] = value

    def evict_page(self, key):
        """Force eviction of the page containing key.  May silently fail (EBUSY)."""
        try:
            evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
            self.session.begin_transaction()
            evict.set_key(key)
            evict.search()
            evict.reset()
            evict.close()
            self.session.rollback_transaction()
        except Exception:
            try:
                self.session.rollback_transaction()
            except Exception:
                pass

    def test_failed_install_retry_no_leak(self):
        # Aggressive workload; expect ~1 minute.
        nrows = 5000
        band = 200
        cycles = 400
        checkpoint_every = 10

        self.session.create(self.uri, self.table_config)

        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_initial = self.get_checkpoint_size()
        self.assertGreater(size_initial, 0)

        # Warm-up: confirm the configuration is hitting the split-eviction path.
        for i in range(10):
            start = (i * band) % nrows
            char = chr(ord('B') + (i % 20))
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, start, band, char)
            c.close()
            self.evict_page(f'key{start:08d}')

        self.session.checkpoint()
        split_leaf_warmup = self.get_conn_stat(stat.conn.cache_eviction_split_leaf)
        split_failed_lock_warmup = self.get_conn_stat(stat.conn.cache_evict_split_failed_lock)
        self.assertGreater(split_leaf_warmup, 0,
            'cache_eviction_split_leaf=0 after warm-up; '
            'leaf_page_max / value size may be too generous to trigger split-eviction')

        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_eviction_split]')

        # Walk through bands and append fresh ranges so pages keep growing past
        # leaf_page_max (keeps the multi-block split path running). Double-evict
        # each band: first try may EBUSY under the failpoint and strand the
        # reconciliation multi-block state; the next dirty cycle must discard it
        # cleanly. Periodic checkpoints flush the post-reconciliation chain
        # discard -- the spot where any accounting drift would trip the
        # running-total decrement assertion.
        for cycle in range(cycles):
            start = (cycle * band) % nrows
            new_start = nrows + cycle * band
            char = chr(ord('a') + (cycle % 20))

            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, start, band, char)
            self.insert_rows(c, new_start, band, char)
            c.close()

            self.evict_page(f'key{start:08d}')
            self.evict_page(f'key{start:08d}')
            self.evict_page(f'key{new_start:08d}')

            if cycle % checkpoint_every == checkpoint_every - 1:
                self.session.checkpoint()

        self.conn.reconfigure('timing_stress_for_test=[]')

        self.session.checkpoint()
        size_final = self.get_checkpoint_size()

        split_failed_lock_final = self.get_conn_stat(stat.conn.cache_evict_split_failed_lock)

        self.assertGreater(split_failed_lock_final, split_failed_lock_warmup,
            f'cache_evict_split_failed_lock did not advance under failpoint '
            f'(warmup={split_failed_lock_warmup}, final={split_failed_lock_final})')

        # Legitimate growth is ~(nrows + cycles*band)/nrows; allow 3x that to
        # accommodate B-tree overhead and delta accumulation. A real leak
        # would push the ratio orders of magnitude higher.
        legitimate_growth = (nrows + cycles * band) / nrows
        max_allowed_ratio = legitimate_growth * 3
        self.assertLess(size_final, size_initial * max_allowed_ratio,
            f'Checkpoint size {size_final} ({size_final / size_initial:.1f}x baseline '
            f'{size_initial}) exceeds expected upper bound {max_allowed_ratio:.1f}x '
            f'-- possible leak in failed-install retry path (cycles ran: {cycles})')

        expected_total = nrows + cycles * band
        c = self.session.open_cursor(self.uri)
        count = sum(1 for _ in c)
        c.close()
        self.assertEqual(count, expected_total,
            f'Row count mismatch: expected {expected_total}, got {count}')

        # Verify the layered table. WT's verify recomputes the on-disk block
        # layout and cross-checks against the metadata-recorded size; drift
        # accumulated in the running total would surface as a verify failure
        # even if the running-total decrement assertion never tripped.
        # Try past transient EBUSY while dirty state is still flushing.
        self.verifyUntilSuccess(uri=self.uri)
