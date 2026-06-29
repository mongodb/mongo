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

# test_disagg_checkpoint_size18.py
#   Exercises the pre-page-log write failure path for delta writes by enabling
#   failpoint_page_log_handle_put. The failpoint fires inside the disagg write
#   path immediately before the page-log put, so the write fails with EBUSY
#   before any data reaches the page log.
#
#   At error time:
#     - the persistent flag is false (write never reached plh_put)
#     - the address cookie is NULL (addr never packed)
#     - the running total is unchanged (running-total increment never called)
#     - the page's persistent flag is true (old chain intact)
#
#   The reconciliation error path's outer persistent check evaluates to false,
#   so the disagg-specific block is skipped and the page's block metadata is
#   left unmodified. The next reconciliation must therefore pass the old block
#   metadata to the disagg write path so the running-total decrement runs
#   before the running-total increment.
#
#   Regression target: if the page's persistent flag were incorrectly set to
#   false after this pre-page-log failure, the next reconciliation would skip
#   the running-total decrement and leak the old chain's cumulative size into
#   the running total. Catches any accounting drift because verify recomputes
#   the on-disk size from blocks and cross-checks it against the
#   metadata-recorded size.

@disagg_test_class
class test_disagg_checkpoint_size18(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size18'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90),'
        'statistics=(all)'
    )
    uri = 'layered:' + uri_base
    stable_uri = 'file:' + uri_base + '.wt_stable'

    def insert_rows(self, cursor, start, count, value_char):
        for i in range(start, start + count):
            cursor[f'key{i:06d}'] = value_char * 200

    def evict_page(self, key):
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_rec_write_err_pre_page_log_delta_failure(self):
        nrows = 20

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Initial full-image checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_initial = self.get_checkpoint_size()
        self.assertGreater(size_initial, 0)

        # Delta checkpoint to build a chain with a non-zero cumulative size.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_initial,
            'Size should grow after a delta checkpoint')

        # Evict so the page is re-loaded from the page service on next access
        # with the persistent flag set to true.
        self.evict_page('key000000')

        # Enable the failpoint. It fires at 1% of writes (probability=100 in
        # the [0,10000] failpoint range), so the loop below iterates until the
        # stat increments.
        self.conn.reconfigure(
            'timing_stress_for_test=[failpoint_page_log_handle_put]')

        # Dirty + evict until the failpoint fires. Each failing write enters
        # the reconciliation error path with the persistent flag false;
        # the disagg block is skipped and the page's block metadata is left
        # unmodified.
        stat_key = stat.dsrc.disagg_block_plh_put_failed
        max_iters = 500
        for i in range(max_iters):
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, chr(ord('C') + (i % 20)))
            c.close()
            self.evict_page('key000000')
            if self.get_stat(stat_key) > 0:
                break
        else:
            self.fail(
                f'failpoint_page_log_handle_put never triggered after {max_iters} evictions')

        # Switch to full-image writes BEFORE disabling the failpoint so the
        # recovery checkpoint forces a fresh full image and exercises the
        # running-total decrement / running-total increment path.
        # Doing it in this order avoids a race where background eviction
        # between reconfigures could write a delta at the old delta_pct=90.
        self.conn.reconfigure('page_delta=(delta_pct=1)')
        self.conn.reconfigure('timing_stress_for_test=[]')

        # Recovery checkpoint: the page's persistent flag being true passes the
        # old block metadata, so the running-total decrement runs before the
        # running-total increment.
        self.session.checkpoint()

        self.conn.reconfigure(
            'page_delta=(delta_pct=90)')

        self.assertGreater(self.get_stat(stat_key), 0,
            'disagg_block_plh_put_failed should be > 0')

        # The strong check: verify recomputes the on-disk block layout and
        # cross-checks it against the metadata-recorded size. Any drift in
        # the running total from a missed running-total decrement would surface
        # here as a verify failure. Verify retries past transient EBUSY while
        # dirty state is still flushing.
        self.verifyUntilSuccess(uri=self.uri)

        c = self.session.open_cursor(self.uri)
        row_count = sum(1 for _ in c)
        c.close()
        self.assertEqual(row_count, nrows,
            f'Expected {nrows} rows after recovery, found {row_count}')
