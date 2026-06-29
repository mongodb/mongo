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
from helper_disagg import DisaggSizeTestMixin, disagg_test_class

# test_disagg_checkpoint_size17.py
#   A checkpoint-cursor open of a disagg btree used to unconditionally
#   initialise the running total from the btree open path, storing the
#   historical ckpt.size into the block state shared with the live writer
#   handle and clobbering the live running total. The next
#   running-total decrement then underflowed and aborted.

@disagg_test_class
class test_disagg_checkpoint_size17(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size17'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=20,leaf_page_delta=true,max_consecutive_delta=10),'
        'cache_size=20MB,'
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
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_checkpoint_cursor_does_not_clobber_live_size(self):
        nrows = 2000

        self.session.create(self.uri, self.table_config)

        # Checkpoint while empty so ckpt.size for the stable file is ~0. This
        # is the historical checkpoint the cursor will open against.
        self.session.checkpoint()

        # Grow the live running total well above the empty checkpoint value.
        # No checkpoint between here and the cursor open so the live state
        # stays divergent.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        self.insert_rows(c, 0, nrows, 'B')
        self.insert_rows(c, 0, nrows, 'C')
        c.close()

        # Opening a checkpoint cursor on the stable file routes through the
        # btree open path. That path must NOT store the historical ckpt.size
        # into the block state shared with the live writer.
        ckpt_c = self.session.open_cursor(self.stable_uri, None,
                                          'checkpoint=WiredTigerCheckpoint')
        ckpt_c.close()

        # Force evictions that subtract cookie bytes via the post-reconciliation
        # chain discard. If the running total had been clobbered by the
        # checkpoint-cursor open, the running-total decrement assertion would
        # abort here.
        for k in (0, nrows // 2, nrows, nrows + nrows // 2):
            self.evict_page(f'key{k:08d}')

        self.session.checkpoint()

        c = self.session.open_cursor(self.uri)
        count = sum(1 for _ in c)
        c.close()
        self.assertEqual(count, nrows,
            f'Row count mismatch after checkpoint-cursor-open + eviction: '
            f'expected {nrows}, got {count}')
