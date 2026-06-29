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

# test_disagg_checkpoint_size09.py
#   Exercises the page re-instantiation path for the persistent flag invariant.
#
#   When eviction encounters a page with uncommitted updates that cannot be
#   moved to the history store, it falls back to the save-update-restore path:
#   the page is kept in memory via the split-rewrite path -> the page
#   re-instantiation path, which instantiates a new in-memory page from the
#   existing disk image.
#
#   After copying the reconciled block's metadata into the page's block metadata,
#   the page re-instantiation path must restore the persistent flag =
#   (cumulative size > 0): the copy site leaves the flag false even though
#   the on-disk delta chain is already counted in the file's running byte total.
#   This matches the same restore pattern applied at the two reconciliation commit
#   path sites.
#
#   Path exercised:
#     1. Build a delta chain so the chain's cumulative size > 0 on disk.
#     2. An uncommitted write on the page forces the save-update-restore path on
#        eviction. The page re-instantiation path instantiates the new page with
#        the persistent flag set.
#     3. Rolling back the uncommitted write leaves the page dirty with only
#        aborted updates; the committed value is a durable update.
#     4. Checkpoint: newer_updates_than_last_rec_used stays false so skip-write
#        fires. The skip-write branch of the reconciliation commit path sets a
#        single-page replacement result and restores the persistent flag at its
#        own copy site.
#     5. Enable failpoint_rec_before_wrapup, dirty the page, and force eviction.
#        The failpoint fires after the full-image write but before the
#        reconciliation commit path, invoking the reconciliation error path with
#        a single-page replacement result + cumulative size > 0 + persistent flag
#        set -- if the persistent flag had been left false, the persistent flag
#        assertion in the running-total decrement would trip.
#
#   Verification:
#     - The reconciled pages scrubbed and added back to the cache clean scenario, confirmed via
#       its stat counter, shows the page re-instantiation path was reached.
#     - The free page ID due to failed page replacement reconciliation scenario, confirmed via
#       its stat counter, shows the reconciliation error path ran without crashing.
#     - Checkpoint size after recovery is not inflated by a leaked chain's
#       cumulative size.

@disagg_test_class
class test_disagg_checkpoint_size09(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size09'
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
    # test_split_multi_inmem_aggregated_flag
    # -----------------------------------------------------------------------
    # Regression for the page re-instantiation path fix.
    #
    # The page re-instantiation path is called from the split-rewrite path when
    # eviction produces a reconciliation's multi-block state with exactly one entry
    # that has the save-update-restore path flag set. This happens when the page
    # has uncommitted updates that cannot be written to the history store.
    #
    # Flow:
    #   1. Write initial rows and checkpoint (full-image baseline).
    #   2. Partially update rows and checkpoint (builds delta chain,
    #      chain's cumulative size > 0).
    #   3. In a loop until both signal stats fire:
    #      a. Open session_a and write uncommitted updates to the page.
    #         Evict the page. Because uncommitted updates from session_a cannot
    #         go to the history store, eviction falls back to the save-update-restore
    #         path and calls the page re-instantiation path. The reconciled pages
    #         scrubbed and added back to the cache clean scenario is exercised.
    #      b. Rollback session_a. The page retains only aborted updates plus
    #         the previously committed 'B' value as a durable update.
    #      c. Checkpoint. The durable 'B' value is unchanged, so
    #         newer_updates_than_last_rec_used stays false and skip-write fires.
    #         The skip-write branch of the reconciliation commit path sets a
    #         single-page replacement result and restores the persistent flag =
    #         (cumulative size > 0).
    #      d. Enable failpoint_rec_before_wrapup + delta_pct=1. Write new
    #         committed data to dirty the page and force a full-image eviction.
    #         The failpoint fires ~1% of the time, invoking the reconciliation
    #         error path with delta count=0, a single-page replacement result,
    #         and cumulative size > 0. With the persistent flag set, the error
    #         path completes; rec_free_page_id increments.
    #   4. Run a final checkpoint and verify size is not inflated.
    def test_split_multi_inmem_aggregated_flag(self):
        nrows = 20
        scrub_stat = stat.dsrc.cache_scrub_restore
        err_stat = stat.dsrc.rec_free_page_id_due_to_failed_replacement_reconciliation

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: initial full-image write + checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_baseline = self.get_checkpoint_size()
        self.assertGreater(size_baseline, 0)

        # Step 2: partial update + checkpoint to build a delta chain so
        # the chain's cumulative size > 0 on disk.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_baseline,
            'Expected checkpoint size to grow after a delta write')

        # Step 3: loop until the page re-instantiation path has been called at least
        # once AND the reconciliation error path has been reached at least once.
        max_iters = 500
        for i in range(max_iters):
            # (a) Uncommitted write forces the save-update-restore path on eviction.
            #     Uncommitted updates from session_a cannot be written to the history
            #     store (they may still be rolled back), so eviction keeps the page
            #     in memory via the page re-instantiation path, exercising the
            #     reconciled pages scrubbed and added back to the cache clean scenario.
            session_a = self.conn.open_session()
            session_a.begin_transaction()
            ca = session_a.open_cursor(self.uri)
            for j in range(nrows // 2):
                ca[f'key{j:06d}'] = chr(ord('C') + (i % 20)) * 200
            ca.close()
            self.evict_page('key000000')

            # (b) Rollback: uncommitted updates become aborted. The page now has
            #     only the committed 'B' value (a durable update) plus aborted updates.
            session_a.rollback_transaction()
            session_a.close()

            # (c) Checkpoint: skip-write fires because newer_updates_than_last_rec_used
            #     stays false (the only visible update is already a durable update).
            #     The reconciliation commit path sets a single-page replacement result
            #     and restores the persistent flag.
            self.session.checkpoint()

            # (d) Enable the failpoint and force a full-image eviction.
            #     The committed write makes the page dirty; delta_pct=1 forces a
            #     full-image write; the failpoint fires ~1% of the time.
            self.conn.reconfigure(
                'page_delta=(delta_pct=1),'
                'timing_stress_for_test=[failpoint_rec_before_wrapup]'
            )
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, chr(ord('D') + (i % 20)))
            c.close()
            self.evict_page('key000000')
            self.conn.reconfigure('timing_stress_for_test=[]')

            if self.get_stat(scrub_stat) > 0 and self.get_stat(err_stat) > 0:
                break
        else:
            self.fail(
                f'Failed to trigger both the save-update-restore path (page '
                f're-instantiation) and the reconciliation error path after '
                f'{max_iters} iterations'
            )

        # Step 4: final checkpoint after the error path has run.
        self.session.checkpoint()
        size_after_recovery = self.get_checkpoint_size()

        # The page re-instantiation path was called (the save-update-restore path ran).
        self.assertGreater(self.get_stat(scrub_stat), 0,
            'cache_scrub_restore should be > 0: page re-instantiation path was not reached')

        # The reconciliation error path ran without crashing.
        self.assertGreater(self.get_stat(err_stat), 0,
            'rec_free_page_id_due_to_failed_replacement_reconciliation should be > 0 '
            'after running with failpoint_rec_before_wrapup')

        # Checkpoint size is not inflated. The reconciliation error path must invoke
        # the delta chain discard; otherwise the chain's cumulative size would leak
        # into the file's running byte total on every error-path hit.
        self.assertLess(size_after_recovery, size_with_delta + size_baseline,
            f'Checkpoint size {size_after_recovery} after recovery is inflated: '
            f'baseline={size_baseline}, after_delta={size_with_delta}. '
            f'Old delta chain cumulative size may have been double-counted.')
