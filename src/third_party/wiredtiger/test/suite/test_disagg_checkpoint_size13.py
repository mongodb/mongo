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

# test_disagg_checkpoint_size13.py
#   Exercises the multi-block error-cleanup loop in the reconciliation error
#   path on the disagg path by driving failpoint_rec_before_wrapup. The
#   failpoint fires AFTER the block write but BEFORE the reconciliation commit
#   path records state, so reconciliation falls into the error path with each
#   chunk's address cookie already populated -- the error-cleanup loop then
#   frees every chunk.

@disagg_test_class
class test_disagg_checkpoint_size13(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size13'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=32),'
        'cache_size=2GB,'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base
    # Small leaf_page_max forces wide N-way splits via the multi-block split;
    # large memory_page_max lets the in-memory page absorb many leaf_page_max
    # worth of data before reconciliation, so the error-cleanup loop covers
    # many chunks per call.
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

    def test_rec_write_err_path(self):
        nrows = 2000
        cycles = 30
        band = 200
        # Big fresh batches per cycle: with memory_page_max=200MB the tail
        # page absorbs ~2 MB at 1 KB/row, then reconciles into ~500 chunks at
        # leaf_page_max=4KB. If failpoint_rec_before_wrapup fires during one
        # of these wide reconciliations, the reconciliation error path cleans
        # up all chunks.
        big_batch = 2000

        self.session.create(self.uri, self.table_config)

        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()

        for i in range(10):
            start = (i * band) % nrows
            char = chr(ord('B') + (i % 20))
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, start, band, char)
            c.close()
            self.evict_page(f'key{start:08d}')
        self.session.checkpoint()

        rec_free_pageid_warmup = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)

        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_rec_before_wrapup]')

        for i in range(cycles):
            char = chr(ord('a') + (i % 20))
            new_start = nrows + i * big_batch
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, new_start, big_batch, char)
            band_start = ((i + cycles) * band) % nrows
            self.insert_rows(c, band_start, band, char)
            c.close()
            self.evict_page(f'key{new_start:08d}')
            self.evict_page(f'key{band_start:08d}')

        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()

        rec_free_pageid_final = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)

        # failpoint_rec_before_wrapup fires probabilistically (1%). If the
        # workload happens not to roll the failpoint, skip rather than fail.
        if rec_free_pageid_final == rec_free_pageid_warmup:
            self.skipTest('failpoint_rec_before_wrapup did not fire in this run')
