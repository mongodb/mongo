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

# test_disagg_checkpoint_size10.py
#   Regression test for the multiblock old-state fix in the reconciliation
#   commit path.
#
#   Bug: when a disagg page carries a prior single-page replacement result (set
#   by a prior save-update-restore eviction that produced exactly one block) and
#   the split-rewrite path subsequently fails, the page stays in the multiblock
#   state. If the next reconciliation writes a full-image reusing the same
#   page_id (without freeing the old page via the post-reconciliation chain
#   discard), the delta chain discard was never invoked for the old chain's
#   cumulative size, permanently inflating the running total on every such cycle.
#
#   The fix adds the same delta chain discard call to the multiblock case in
#   the reconciliation commit path that was already present for the no-prior-state
#   and single-page replacement cases, subject to the same accounting guard:
#   the reconciliation's multi-block state has exactly one entry, the skip-write
#   flag is clear, and the delta count is zero.
#
#   Exact triggering path:
#     1. Eviction falls back to the save-update-restore path for a disagg page
#        with uncommitted updates: the reconciliation commit path records a
#        multiblock result with one entry. The entry's chain's cumulative size
#        S1 is counted in the running total.
#     2. The split-rewrite path fails (e.g., allocation error). The page stays
#        in the multiblock state; S1 remains counted.
#     3. Uncommitted updates are rolled back.
#     4. A subsequent checkpoint or eviction reconciles the page cleanly (no
#        uncommitted updates), writing a full-image that reuses the same page_id.
#        The reconciliation commit path enters the multiblock case as the old
#        state and must subtract S1 via the delta chain discard; if it doesn't,
#        S1 leaks indefinitely.
#
#   This test verifies the broader accounting invariant: after repeated
#   save-update-restore eviction cycles followed by full-image checkpoint writes,
#   checkpoint size remains stable and does not grow with each cycle.
#   The reconciled pages scrubbed and added back to the cache clean scenario, confirmed via its
#   stat counter, shows the save-update-restore path was reached (step 1).

@disagg_test_class
class test_disagg_checkpoint_size10(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size10'
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
    # test_multiblock_to_replace_size_stable
    # -----------------------------------------------------------------------
    # Regression for the multiblock case in the reconciliation commit path
    # delta chain discard fix.
    #
    # Flow:
    #   1. Write initial rows and checkpoint (full-image baseline).
    #   2. Partial update + checkpoint to build a delta chain so
    #      the chain's cumulative size is > 0 on disk.
    #   3. Loop until the reconciled pages scrubbed and added back to the cache clean scenario
    #      fires (per its stat counter):
    #      a. Open session_a and write uncommitted updates to the page.
    #         Evict the page. Uncommitted updates cannot go to the history
    #         store, so eviction falls back to the save-update-restore path,
    #         exercising the reconciled pages scrubbed and added back to the cache clean path.
    #      b. Rollback session_a.
    #      c. Switch to delta_pct=1 to force a full-image write, then
    #         run a checkpoint. The reconciliation commit path enters the
    #         split-rewrite case and invokes the delta chain discard for
    #         the old chain's cumulative size S1.
    #   4. After enough cycles, verify checkpoint size is stable (not growing
    #      by S1 each cycle).
    def test_multiblock_to_replace_size_stable(self):
        nrows = 20
        scrub_stat = stat.dsrc.cache_scrub_restore

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: initial full-image write + checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_baseline = self.get_checkpoint_size()
        self.assertGreater(size_baseline, 0)

        # Step 2: partial update + checkpoint to build a delta chain.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()
        self.assertGreater(size_with_delta, size_baseline,
            'Expected checkpoint size to grow after a delta write')

        # Step 3: loop until the save-update-restore path is triggered at least once.
        max_iters = 200
        for i in range(max_iters):
            # (a) Uncommitted write forces the save-update-restore path on eviction.
            session_a = self.conn.open_session()
            session_a.begin_transaction()
            ca = session_a.open_cursor(self.uri)
            for j in range(nrows // 2):
                ca[f'key{j:06d}'] = chr(ord('C') + (i % 20)) * 200
            ca.close()
            self.evict_page('key000000')

            # (b) Rollback: uncommitted updates become aborted.
            session_a.rollback_transaction()
            session_a.close()

            # (c) Force a full-image write (delta_pct=1) and checkpoint.
            #     Writes committed data to dirty the page, then checkpoints.
            self.conn.reconfigure('page_delta=(delta_pct=1)')
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, chr(ord('D') + (i % 20)))
            c.close()
            self.session.checkpoint()
            self.conn.reconfigure(
                'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=10)'
            )

            if self.get_stat(scrub_stat) > 0:
                break
        else:
            self.fail(
                f'Failed to trigger the save-update-restore path '
                f'after {max_iters} iterations'
            )

        # Step 4: run a few more full-image checkpoints to confirm size stability.
        # If the multiblock old-state path failed to subtract the old cumulative S1,
        # the running total would grow on every cycle, making size_final >> size_with_delta.
        for _ in range(5):
            self.conn.reconfigure('page_delta=(delta_pct=1)')
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, 'Z')
            c.close()
            self.session.checkpoint()
            self.conn.reconfigure(
                'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=10)'
            )

        size_final = self.get_checkpoint_size()

        # The reconciled pages scrubbed and added back to the cache clean scenario, confirmed via
        # its stat counter, shows the save-update-restore path was reached.
        self.assertGreater(self.get_stat(scrub_stat), 0,
            'cache_scrub_restore should be > 0: the save-update-restore path was not triggered')

        # Checkpoint size should not be inflated beyond a reasonable bound.
        # Each full-image write replaces the previous delta chain; if the delta chain
        # discard is called correctly, the size reflects only the current page content.
        # Allow 2x size_with_delta as headroom for implementation overhead.
        self.assertLess(size_final, 2 * size_with_delta,
            f'Checkpoint size {size_final} after repeated save-update-restore + full-image cycles '
            f'is inflated beyond 2x the delta baseline {size_with_delta}. '
            f'The old delta chain cumulative size may have been double-counted.')
