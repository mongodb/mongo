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

# test_disagg_checkpoint_size08.py
#   Exercises the persistent flag invariant in two reconciliation commit paths:
#
#   Path 1 -- the save-update-restore path in the reconciliation commit path:
#     When eviction keeps a page in memory with restored updates (because an
#     active reader transaction prevents those updates from becoming globally
#     visible), it copies the reconciled block's metadata into the page's block
#     metadata without restoring the persistent flag. The copy site reset leaves
#     the flag false even though the on-disk size is still counted in the
#     file's running byte total.
#
#   Path 2 -- skip-write in the reconciliation commit path (single-block replace path):
#     When eviction reuses the page's existing on-disk address (content unchanged
#     since the last write), the address copy path also resets the persistent flag
#     in the reconciled block's metadata. The reconciliation commit path then copies
#     that false value into the page's block metadata while setting a single-page
#     replacement result.
#
#   In both cases a subsequent failed full-image write in the reconciliation error
#   path hit an assertion on the persistent flag and aborted the process.
#
#   The fix restores the persistent flag = (cumulative size > 0) after the struct
#   copy at both reconciliation commit path sites, matching the same pattern already
#   applied in the page re-instantiation path.
#
#   Tests:
#     test_supd_restore_wrapup_aggregated_flag
#       A long-running reader session pins an early read timestamp, preventing
#       updates written at later timestamps from becoming globally visible. This
#       forces the save-update-restore path during eviction of dirty pages. With
#       timing_stress_for_test=[failpoint_rec_before_wrapup] enabled, a later
#       failed full-image write exercises the assertion path. Verifies:
#         - The process does not abort (assertion is not reached).
#         - The free page ID due to failed page replacement reconciliation scenario, confirmed
#           via its stat counter, shows the reconciliation error path ran at least once.
#         - Checkpoint size is not inflated by a leaked chain's cumulative size.
#
#     test_skip_write_wrapup_aggregated_flag
#       Builds a delta chain (chain's cumulative size > 0), then writes to the page
#       and rolls back, leaving a dirty page with only aborted updates. A checkpoint
#       fires skip-write (newer_updates_than_last_rec_used is false because the
#       committed on-page update is a durable update). The skip-write path in the
#       reconciliation commit path must restore the persistent flag =
#       (cumulative size > 0).
#       With failpoint_rec_before_wrapup, the next eviction enters the reconciliation
#       error path and would hit the assertion if the persistent flag were left false.
#       Verifies the same invariants as the save-update-restore test.

@disagg_test_class
class test_disagg_checkpoint_size08(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size08'
    # delta_pct=90: small updates produce deltas; switched to 1 when a full image is needed.
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=10)'
    )
    uri = 'layered:' + uri_base
    stable_uri = 'file:' + uri_base + '.wt_stable'

    # -----------------------------------------------------------------------
    # Helpers
    # -----------------------------------------------------------------------

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
    # test_supd_restore_wrapup_aggregated_flag
    # -----------------------------------------------------------------------
    # Regression for the save-update-restore path in the reconciliation commit path.
    #
    # The reconciliation commit path copies the reconciled block's metadata (where the
    # copy site has reset the persistent flag to false) into the page's block
    # metadata, then must restore the persistent flag = (cumulative size > 0).
    # Without that restore, a subsequent skip-write reconciliation propagates
    # the false flag into the single-page replacement result, and a later failed
    # full-image write trips the persistent flag assertion in the running-total
    # decrement.
    #
    # Setup:
    #   1. Write initial rows and checkpoint (full-image baseline).
    #   2. Partially update rows and checkpoint (builds delta chain,
    #      chain's cumulative size > 0).
    #   3. Open a blocker session that pins an old read timestamp, preventing
    #      updates at newer timestamps from becoming globally visible.
    #   4. Continue writing at new timestamps; eviction sees pages with updates
    #      that cannot be flushed to the history store (blocked), triggering
    #      the save-update-restore path to keep the updates in memory.
    #   5. Enable failpoint_rec_before_wrapup so a later full-image write fails
    #      and enters the reconciliation error path with the invariant conditions.
    #   6. Verify no abort, stat > 0, and size is not inflated.
    def test_supd_restore_wrapup_aggregated_flag(self):
        nrows = 20
        stat_key = stat.dsrc.rec_free_page_id_due_to_failed_replacement_reconciliation

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: initial full-image baseline.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')
        self.session.checkpoint()
        size_baseline = self.get_checkpoint_size()
        self.assertGreater(size_baseline, 0)

        # Step 2: partial update to build a delta chain (chain's cumulative size > 0).
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.commit_transaction('commit_timestamp=2')
        self.conn.set_timestamp('stable_timestamp=2')
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_baseline,
            'Expected checkpoint size to grow after a delta write')

        # Step 3: blocker session pins read_timestamp=1. Updates at ts>=2 are
        # not globally visible while this transaction is open, so eviction of pages
        # carrying those updates cannot write them to the history store and falls
        # back to the save-update-restore path (restores updates into the in-memory page).
        blocker = self.conn.open_session()
        blocker.begin_transaction('read_timestamp=1')

        # Evict the page so the next read re-loads it from disk with the delta-chain
        # block metadata (chain's cumulative size > 0, persistent flag set).
        self.evict_page('key000000')

        # Step 45: enable failpoint and loop until the reconciliation error path is reached.
        # delta_pct=1 forces full-image writes; the blocker transaction forces
        # the save-update-restore path on the first eviction attempt of pages with ts>=2
        # updates. After the save-update-restore path the page remains in cache with the
        # reconciliation's multi-block state; a subsequent skip-write turns it to a
        # single-page replacement result. The failpoint then fires during the next
        # full-image write -- the reconciliation error path would trip the persistent flag
        # assertion if the reconciliation commit path did not restore the flag.
        self.conn.reconfigure(
            'page_delta=(delta_pct=1),'
            'timing_stress_for_test=[failpoint_rec_before_wrapup]'
        )
        max_iters = 500
        for i in range(max_iters):
            ts = 3 + i
            c = self.session.open_cursor(self.uri)
            self.session.begin_transaction()
            self.insert_rows(c, 0, nrows, chr(ord('C') + (i % 20)))
            c.close()
            self.session.commit_transaction(f'commit_timestamp={ts}')
            self.conn.set_timestamp(f'stable_timestamp={ts}')
            self.evict_page('key000000')
            if self.get_stat(stat_key) > 0:
                break

        # Release the blocker before cleanup so eviction is unblocked.
        blocker.rollback_transaction()
        blocker.close()

        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()
        size_after_recovery = self.get_checkpoint_size()

        # The failpoint fires probabilistically (1% per reconcile). With a bounded
        # workload it may not trigger on a given run; skip the size check below
        # in that case rather than asserting on a probability.
        if self.get_stat(stat_key) == 0:
            self.skipTest('failpoint_rec_before_wrapup did not fire in this run')

        # Verify size is not inflated. Without the fix, the save-update-restore path
        # left the persistent flag false; the skip-write path then set a single-page
        # replacement result with the wrong flag; the reconciliation error path skipped
        # the delta chain discard, leaking the chain's cumulative size into the file's
        # running byte total on every error-path iteration.
        self.assertLess(size_after_recovery, size_with_delta + size_baseline,
            f'Checkpoint size {size_after_recovery} after recovery is inflated: '
            f'baseline={size_baseline}, after_delta={size_with_delta}. '
            f'Old delta chain cumulative size may have been double-counted.')

    # -----------------------------------------------------------------------
    # test_skip_write_wrapup_aggregated_flag
    # -----------------------------------------------------------------------
    # Regression for the skip-write path in the reconciliation commit path
    # (single-block replace branch).
    #
    # The skip-write path (the skip-write flag) copies the reconciled block's metadata
    # (where the address copy path has reset the persistent flag to false) into the
    # page's block metadata and sets a single-page replacement result. It must then
    # restore the persistent flag = (cumulative size > 0) so the flag reflects that
    # the size IS counted in the file's running byte total (skip-write reuses the
    # existing on-disk address without subtracting or re-adding to the running total).
    # Otherwise a subsequent failed eviction write trips the persistent flag assertion
    # in the running-total decrement.
    #
    # Skip-write fires when:
    #   - last_block && single-page (single-page reconciliation result)
    #   - address cookie has a valid page_id (page written before)
    #   - a single-page reconciliation result (the reconciliation result is REPLACE)
    #   - !newer_updates_than_last_rec_used (no new committed updates)
    #
    # To reliably trigger skip-write during CHECKPOINT (so the page stays in cache
    # with the wrong flag set by the reconciliation commit path):
    #   1. Write data and checkpoint: page has REPLACE result, chain's cumulative
    #      size > 0. The on-page committed updates become durable updates.
    #   2. Write to the page and ROLLBACK. The rolled-back update is aborted and
    #      invisible to reconciliation. The page is dirty (has a modify struct)
    #      but all visible committed updates were already written as durable updates.
    #   3. Checkpoint. Reconciliation selects the committed durable update as the
    #      on-page update. Because it is already durable, newer_updates_than_last_rec_used
    #      stays false and skip-write fires.
    #      The reconciliation commit path copies the reconciled block's metadata (where
    #      the address copy path reset the persistent flag to false) into the page's
    #      block metadata and must restore the persistent flag to true.
    #      The page remains in cache with a single-page replacement result.
    #   4. Enable failpoint_rec_before_wrapup and evict with delta_pct=1.
    #      The eviction writes a full-image (delta count=0); the failpoint fires and
    #      the reconciliation error path finds a single-page replacement result +
    #      cumulative size > 0. If the persistent flag had been left false the
    #      assertion would trip here.
    def test_skip_write_wrapup_aggregated_flag(self):
        nrows = 20
        stat_key = stat.dsrc.rec_free_page_id_due_to_failed_replacement_reconciliation

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: initial full-image baseline to establish page_id and REPLACE result.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_baseline = self.get_checkpoint_size()
        self.assertGreater(size_baseline, 0)

        # Step 2: write a delta to create a chain's cumulative size > 0 on disk.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_baseline,
            'Expected checkpoint size to grow after a delta write')

        # Steps 34: loop until the error path fires.
        # Each iteration:
        #   a. Write to the page and rollback. The page becomes dirty with only
        #      aborted updates; the committed 'B' data is still a durable update.
        #   b. Checkpoint. Skip-write fires: newer_updates_than_last_rec_used stays
        #      false because the already-durable 'B' update is unchanged. The
        #      skip-write path in the reconciliation commit path restores the persistent
        #      flag = (cumulative size > 0), reflecting that the chain is still counted
        #      in the file's running byte total.
        #   c. Enable failpoint + delta_pct=1 and write committed data.
        #   d. Evict: full-image write -> failpoint fires -> reconciliation error path.
        #      With the persistent flag set on entry, the error path completes; the
        #      page_id is invalidated and the rec_free_page_id stat increments.
        max_iters = 500
        for i in range(max_iters):
            # (a) Write + rollback: dirty page, only aborted updates, content unchanged.
            c = self.session.open_cursor(self.uri)
            self.session.begin_transaction()
            self.insert_rows(c, 0, nrows, chr(ord('C') + (i % 20)))
            c.close()
            self.session.rollback_transaction()

            # (b) Checkpoint with skip-write. Before the fix, this set the persistent
            # flag to false on the page, which stayed in cache with a single-page
            # replacement result.
            self.session.checkpoint()

            # (c-d) Enable failpoint and evict with full-image mode.
            self.conn.reconfigure(
                'page_delta=(delta_pct=1),'
                'timing_stress_for_test=[failpoint_rec_before_wrapup]'
            )
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, chr(ord('D') + (i % 20)))
            c.close()
            self.evict_page('key000000')
            self.conn.reconfigure('timing_stress_for_test=[]')

            if self.get_stat(stat_key) > 0:
                break

        self.session.checkpoint()
        size_after_recovery = self.get_checkpoint_size()

        # Verify the error path ran.
        self.assertGreater(self.get_stat(stat_key), 0,
            'rec_free_page_id_due_to_failed_replacement_reconciliation should be > 0 '
            'after skip-write wrapup followed by failpoint eviction')

        # Verify size is not inflated.
        self.assertLess(size_after_recovery, size_with_delta + size_baseline,
            f'Checkpoint size {size_after_recovery} after recovery is inflated: '
            f'baseline={size_baseline}, after_delta={size_with_delta}. '
            f'Old delta chain cumulative size may have been double-counted.')
