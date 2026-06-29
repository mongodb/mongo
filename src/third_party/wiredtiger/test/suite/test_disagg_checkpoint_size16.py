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

# test_disagg_checkpoint_size16.py
#   Exercises the disagg delta-chain invalidation path: a hot single page is
#   re-reconciled many times under failpoint_rec_before_wrapup so each
#   reconciliation is a delta on the same page_id (delta_pct=90,
#   max_consecutive_delta=32). When the failpoint fires, the reconciliation
#   error path's single-block branch invalidates the address cookie via the
#   post-reconciliation chain discard, subtracting the chain's cumulative size
#   from the running total. A second phase runs the same failpoint against a
#   wide multi-block table so the multi-block err-cleanup loop also exercises
#   the post-reconciliation chain discard. If accounting in any prior write
#   didn't increment the running total by the correct amount, the
#   running-total decrement assertion aborts the process.

@disagg_test_class
class test_disagg_checkpoint_size16(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size16'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=32),'
        'cache_size=2GB,'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base

    def insert_rows(self, cursor, start, count, value_char):
        value = value_char * 1024
        for i in range(start, start + count):
            cursor[f'key{i:08d}'] = value

    def evict_page(self, uri, key):
        evict = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_delta_chain_invalidation(self):
        nrows = 50
        cycles = 200
        wide_band = 200
        wide_cycles = 40

        # Phase A table: large leaf_page_max keeps all rows on one page so
        # delta_count accumulates without splits resetting the chain.
        single_page_config = ('key_format=S,value_format=S,'
                              'leaf_page_max=128KB,internal_page_max=8KB,'
                              'memory_page_max=200MB,split_pct=90')
        self.session.create(self.uri, single_page_config)

        # Phase B table: small leaf_page_max forces wide multi-block splits.
        wide_uri = 'layered:' + self.uri_base + '_wide'
        wide_table_config = ('key_format=S,value_format=S,'
                             'leaf_page_max=4KB,internal_page_max=8KB,'
                             'memory_page_max=200MB,split_pct=50')
        self.session.create(wide_uri, wide_table_config)

        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        wc = self.session.open_cursor(wide_uri)
        self.insert_rows(wc, 0, 2000, 'A')
        wc.close()
        self.session.checkpoint()

        rec_free_pageid_warmup = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)

        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_rec_before_wrapup]')

        # Phase A: rewrite the same nrows repeatedly. Each cycle re-reconciles
        # the single page as a delta (delta_pct=90), with a full image every
        # max_consecutive_delta=32 cycles. Some reconciliations fail at the
        # failpoint, exercising the error-path post-reconciliation chain discard.
        for i in range(cycles):
            char = chr(ord('a') + (i % 20))
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, char)
            c.close()
            self.evict_page(self.uri, 'key00000000')

        # Phase B: wide multi-block reconciliations on the small-page table.
        for i in range(wide_cycles):
            char = chr(ord('A') + (i % 20))
            wc = self.session.open_cursor(wide_uri)
            new_start = 2000 + i * wide_band
            self.insert_rows(wc, new_start, wide_band, char)
            self.insert_rows(wc, (i * wide_band) % 2000, wide_band, char)
            wc.close()
            self.evict_page(wide_uri, f'key{new_start:08d}')
            self.evict_page(wide_uri, f'key{(i * wide_band) % 2000:08d}')

        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()

        rec_free_pageid_final = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)

        # failpoint_rec_before_wrapup fires probabilistically (1%). If the
        # workload happens not to roll the failpoint, skip rather than fail.
        if rec_free_pageid_final == rec_free_pageid_warmup:
            self.skipTest('failpoint_rec_before_wrapup did not fire in this run')
