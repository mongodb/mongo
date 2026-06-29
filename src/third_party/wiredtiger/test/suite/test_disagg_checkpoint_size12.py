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

# test_disagg_checkpoint_size12.py
#   Exercises the running total accounting on the multi-block eviction path:
#   leaf pages split under update pressure via the multi-block split, and
#   failpoint_eviction_split routes some splits through the error-cleanup loop
#   in the reconciliation error path. A leak there would underflow the
#   running-total decrement assertion or inflate the recorded checkpoint size
#   unbounded.

@disagg_test_class
class test_disagg_checkpoint_size12(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size12'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=32),'
        'cache_size=2GB,'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base
    stable_uri = 'file:' + uri_base + '.wt_stable'
    # leaf_page_max small + memory_page_max large so the in-memory page absorbs
    # many leaf_page_max worth of data before reconciliation, producing a wide
    # N-way chunked write.
    table_config = ('key_format=S,value_format=S,'
                    'leaf_page_max=4KB,internal_page_max=8KB,'
                    'memory_page_max=200MB,'
                    'split_pct=50')

    def insert_rows(self, cursor, start, count, value_char):
        value = value_char * 1024
        for i in range(start, start + count):
            cursor[f'key{i:08d}'] = value

    def evict_page(self, key):
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_split_leaf_eviction_size_stable(self):
        nrows = 2000
        cycles = 20
        band = 200

        self.session.create(self.uri, self.table_config)

        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_initial = self.get_checkpoint_size()
        self.assertGreater(size_initial, 0)

        # Warm-up: update + evict cycles without the failpoint, to drive
        # leaf pages split during eviction upward.
        for i in range(cycles):
            start = (i * band) % nrows
            char = chr(ord('B') + (i % 20))
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, start, band, char)
            c.close()
            self.evict_page(f'key{start:08d}')

        self.session.checkpoint()
        split_leaf_warmup = self.get_conn_stat(stat.conn.cache_eviction_split_leaf)
        self.assertGreater(split_leaf_warmup, 0,
            'cache_eviction_split_leaf stayed 0 in warm-up; '
            'leaf_page_max / value size may be too generous to trigger split-eviction')

        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_eviction_split]')

        # Stress: existing-band updates plus appended fresh ranges so the
        # keys keep growing and producing leaf-page splits even after
        # existing pages settle. Any running-total accounting drift surfaces
        # as a decrement underflow that aborts the process.
        for i in range(cycles):
            start = ((i + cycles) * band) % nrows
            char = chr(ord('a') + (i % 20))
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, start, band, char)
            new_start = nrows + i * band
            self.insert_rows(c, new_start, band, char)
            c.close()
            self.evict_page(f'key{start:08d}')
            self.evict_page(f'key{new_start:08d}')

        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()
        size_after_stress = self.get_checkpoint_size()

        split_leaf_final = self.get_conn_stat(stat.conn.cache_eviction_split_leaf)
        self.assertGreater(split_leaf_final, 0,
            f'cache_eviction_split_leaf is 0 after both phases -- the test is not '
            f'reaching the split-leaf-eviction path it targets')

        # Generous headroom (8x) for delta accumulation and split overhead.
        # A real leak would inflate this by orders of magnitude across cycles.
        self.assertLess(size_after_stress, size_initial * 8,
            f'Checkpoint size {size_after_stress} after split-evict stress is '
            f'much larger than baseline {size_initial} -- possible '
            f'running-total leak in the multi-block error-cleanup path.')

        expected_total = nrows + cycles * band
        c = self.session.open_cursor(self.uri)
        count = sum(1 for _ in c)
        c.close()
        self.assertEqual(count, expected_total,
            f'Row count mismatch after stress: expected {expected_total}, got {count}')

