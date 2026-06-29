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

# test_disagg_checkpoint_size15.py
#   Adds history-store pressure on top of the reconciliation error path workload
#   by holding an old read_timestamp pinned across many writer cycles, so
#   updates at newer commit_timestamps push prior versions to the history store
#   (WiredTigerSharedHS.wt_stable). Combined with failpoint_rec_before_wrapup,
#   this drives the reconciliation error path on a btree with active HS
#   activity -- the format-stress workload that originally produced the disagg
#   underflow.

@disagg_test_class
class test_disagg_checkpoint_size15(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size15'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=32),'
        'cache_size=2GB,'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base
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

    def test_hs_pressure_rec_write_err(self):
        nrows = 2000
        cycles = 30
        band = 200
        big_batch = 2000

        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')

        self.session.create(self.uri, self.table_config)

        # Initial load at ts=10. After this commit, any update at ts>10 with
        # the reader pinned at ts=10 routes the old value through HS.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Long-running reader session pinning ts=10. Holding this open forces
        # every newer update of an existing row to keep the old version alive
        # in HS. Only the txn is kept open -- a live cursor would pin pages
        # and block release_evict.
        reader_sess = self.conn.open_session()
        reader_sess.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        rc = reader_sess.open_cursor(self.uri)
        rc.set_key('key00000000')
        rc.search()
        rc.close()

        # Warm-up at ts=20: updates of existing rows generate HS entries.
        for i in range(10):
            band_start = (i * band) % nrows
            char = chr(ord('B') + (i % 20))
            self.session.begin_transaction()
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, band_start, band, char)
            c.close()
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(20 + i))
            self.evict_page(f'key{band_start:08d}')
        self.session.checkpoint()

        rec_free_pageid_warmup = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)
        hs_insert_warmup = self.get_conn_stat(stat.conn.cache_hs_insert)

        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_rec_before_wrapup]')

        # Stress: updates at moving commit_timestamps so each cycle pushes
        # the prior version of each band's rows into HS.
        for i in range(cycles):
            ts = 100 + i
            char = chr(ord('a') + (i % 20))
            new_start = nrows + i * big_batch
            band_start = ((i + cycles) * band) % nrows

            self.session.begin_transaction()
            c = self.session.open_cursor(self.uri)
            # Fresh appends do NOT generate HS entries (rows didn't exist at
            # the reader's timestamp).
            self.insert_rows(c, new_start, big_batch, char)
            # Updates of existing rows DO push old versions to HS.
            self.insert_rows(c, band_start, band, char)
            c.close()
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(ts))

            self.evict_page(f'key{new_start:08d}')
            self.evict_page(f'key{band_start:08d}')

        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()

        reader_sess.rollback_transaction()
        reader_sess.close()

        hs_insert_final = self.get_conn_stat(stat.conn.cache_hs_insert)
        rec_free_pageid_final = self.get_conn_stat(
            stat.conn.rec_free_page_id_due_to_failed_replacement_reconciliation)

        self.assertGreater(hs_insert_final, hs_insert_warmup,
            'cache_hs_insert did not advance -- HS pressure was not applied')

        # failpoint_rec_before_wrapup fires probabilistically (1%). If the
        # workload happens not to roll the failpoint, skip rather than fail.
        if rec_free_pageid_final == rec_free_pageid_warmup:
            self.skipTest('failpoint_rec_before_wrapup did not fire in this run')
