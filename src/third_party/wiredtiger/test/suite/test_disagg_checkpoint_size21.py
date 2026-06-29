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

# test_disagg_checkpoint_size21.py
#   Directly exercises the reconciliation error path using failpoint_rec_before_wrapup
#   to inject a failure after a full-image eviction write over an existing delta
#   chain (a single-page replacement result).
#
# Bug: when a full-image write fails in the reconciliation error path for a page that
# had a live delta chain, the old chain's cumulative size was not subtracted from the
# file's running byte total. The next successful checkpoint then saw an invalid page_id
# and skipped the delta chain discard, permanently leaking the chain's cumulative size.
#
# This test enables timing_stress_for_test=[failpoint_rec_before_wrapup], which fires
# 1% of the time during eviction reconciliation (WT_REC_EVICT), and loops until the
# free page ID due to failed page replacement reconciliation scenario increments its
# statistic -- the signal that the error path ran.

@disagg_test_class
class test_disagg_checkpoint_size21(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size21'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=10)'
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

    # -----------------------------------------------------------------------
    # test_rec_write_err_full_image_over_delta
    # -----------------------------------------------------------------------
    # Directly exercises the reconciliation error path with the conditions:
    #   - delta count == 0 (full-image write, from delta_pct=1)
    #   - a single-page replacement result (page has a prior successful reconciliation)
    #   - the chain's cumulative size > 0 (live delta chain on disk)
    #
    # Flow:
    #   1. Write initial data + checkpoint: a single-page replacement result with a full image.
    #   2. Write partial update + checkpoint: delta appended; the chain's cumulative size grows.
    #   3. Evict the leaf page (returns it to the page service).
    #   4. Enable failpoint_rec_before_wrapup + delta_pct=1.
    #   5. Dirty the page and force eviction in a loop. Initial evictions succeed,
    #      re-establishing a single-page replacement result in cache. Eventually
    #      the 1% failpoint fires after the full-image write, entering the reconciliation
    #      error path with all three conditions true and exercising the cleanup path.
    #   6. Disable failpoint and run a final checkpoint.
    #   7. Assert the stat counter is > 0 (error path was reached).
    #   8. Assert the checkpoint size is not inflated by the leaked chain's cumulative size.
    def test_rec_write_err_full_image_over_delta(self):
        nrows = 20

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: Initial full-image checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_initial = self.get_checkpoint_size()
        self.assertGreater(size_initial, 0)

        # Step 2: Append a delta to build a chain with a cumulative size > size_initial.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_initial,
            'Size should grow after appending a delta to the chain')

        # Step 3: Evict the leaf page so it is re-read from the page service on next
        # access. The disk-load path sets the persistent flag.
        self.evict_page('key000000')

        # Step 4: Enable the failpoint and switch to full-image mode.
        # timing_stress_for_test=[failpoint_rec_before_wrapup] fires 1% of the time
        # during eviction reconciliation (WT_REC_EVICT) after the block write but before
        # the reconciliation commit path, setting ret = EBUSY and invoking the
        # reconciliation error path.
        self.conn.reconfigure(
            'page_delta=(delta_pct=1),'
            'timing_stress_for_test=[failpoint_rec_before_wrapup]'
        )

        # Step 5: Dirty the page and force eviction until the failpoint fires.
        # The first several evictions succeed and re-establish a single-page replacement
        # result in cache. Once that is set, a subsequent eviction that hits the 1%
        # failpoint enters the reconciliation error path with all three conditions true
        # and exercises the free page ID due to failed page replacement reconciliation scenario.
        stat_key = stat.dsrc.rec_free_page_id_due_to_failed_replacement_reconciliation
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
                f'failpoint_rec_before_wrapup never triggered the reconciliation '
                f'error path after {max_iters} evictions'
            )

        # Disable failpoint before the successful checkpoint to prevent another error.
        self.conn.reconfigure('timing_stress_for_test=[]')

        # Step 6: Final checkpoint after the error path has run.
        self.session.checkpoint()
        size_after_recovery = self.get_checkpoint_size()

        # Step 7: Stat check: the error path was reached at least once.
        self.assertGreater(self.get_stat(stat_key), 0,
            'rec_free_page_id_due_to_failed_replacement_reconciliation should be > 0')

        # Step 8: Size bound -- the reconciliation error path must clean up the old chain's
        # cumulative size from the file's running byte total, otherwise the recovery
        # checkpoint would record (old_chain + new_block) instead of just the new block.
        self.assertLess(size_after_recovery, size_with_delta + size_initial,
            f'Checkpoint size {size_after_recovery} after error-path recovery is too large '
            f'(size_initial={size_initial}, size_with_delta={size_with_delta}). '
            f'The old delta chain cumulative size may not have been cleaned up in '
            f'the reconciliation error path.')
