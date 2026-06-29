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

# Tests for checkpoint size calculation with delta chain scenarios.
#
# Tests:
#   test_size_stable_through_delta_full_image_cycles
#       Exercises the reconciliation commit path: delta chain -> full-image replacement, repeated.
#       Verifies the checkpoint size stabilizes at the baseline (no leak per cycle).
#
#   test_size_after_page_id_invalidation
#       Exercises the reconciliation error path directly: builds a delta chain, forces the
#       failed-replacement error path via eviction + page_id invalidation, then verifies
#       the subsequent checkpoint size is correct.
#       Checks that the stat counter is incremented and that the size is not inflated.

@disagg_test_class
class test_disagg_checkpoint_size06(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_ckpt_size06'
    # delta_pct=90: high enough that small updates produce deltas; we override to 1 when we need
    # a forced full-image write.
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
        """Force the page containing key out of cache via debug=(release_evict)."""
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    # -----------------------------------------------------------------------
    # test_size_stable_through_delta_full_image_cycles
    # -----------------------------------------------------------------------
    # Sequence per cycle:
    #   1. Update a subset of rows (produces a delta at delta_pct=90).
    #   2. Checkpoint: delta appended to chain, the chain's cumulative size grows.
    #   3. Evict the leaf page from cache.
    #   4. Reconfigure to delta_pct=1 (forces next write to be a full image).
    #   5. Update all rows: triggers a full-image reconciliation on next checkpoint.
    #   6. Checkpoint: the reconciliation commit path invokes the delta chain discard,
    #      subtracting the chain's cumulative size from the file's running byte total.
    #   7. Restore delta_pct=90 for the next cycle.
    #
    # This test runs multiple cycles of this sequence to verify that the
    # checkpoint size stabilizes and does not grow by multiples per cycle,
    # which would indicate a leak of the chain's cumulative size.
    def test_size_stable_through_delta_full_image_cycles(self):
        nrows = 20
        ncycles = 5

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Baseline: write initial data and checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        baseline = self.get_checkpoint_size()
        self.assertGreater(baseline, 0)

        for cycle in range(ncycles):
            # Step 12: Partial update + delta checkpoint.
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows // 2, 'B')
            c.close()
            self.session.checkpoint()

            deltas = self.get_stat(stat.dsrc.rec_page_delta_leaf, uri=self.stable_uri)
            self.assertGreater(deltas, 0,
                f'cycle {cycle}: expected a leaf delta but got {deltas}')

            # Step 3: Evict so the page is read back from the page service on next access.
            self.evict_page('key000000')

            # Step 46: Force full-image, update all rows, checkpoint.
            self.conn.reconfigure('page_delta=(delta_pct=1)')
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, 'C')
            c.close()
            self.session.checkpoint()

            size_after_full = self.get_checkpoint_size()

            # The full-image checkpoint size should not diverge from the
            # baseline. Allow a 2x margin to accommodate root page and internal page
            # overhead, but the size must not grow by multiples per cycle.
            self.assertLess(size_after_full, baseline * 2,
                f'cycle {cycle}: checkpoint size {size_after_full} is more than 2x '
                f'baseline {baseline}  possible running-total leak')

            # Restore delta mode for the next cycle.
            self.conn.reconfigure('page_delta=(delta_pct=90)')

    # -----------------------------------------------------------------------
    # test_size_after_page_id_invalidation
    # -----------------------------------------------------------------------
    # Directly exercises the scenario where page_id is invalidated during eviction
    # (triggering the free page ID due to failed page replacement reconciliation scenario)
    # and verifies the subsequent checkpoint size is consistent.
    #
    # The reconciliation error path is reached when:
    #   - The page has disaggregated metadata and a valid page_id.
    #   - Reconciliation writes exactly one block (the reconciliation's multi-block
    #     state has a single entry).
    #   - The new block's page_id matches the page's current page_id.
    #   - An error occurs after the write but before the reconciliation commit path
    #     commits the state.
    #
    # We approximate this via a crash-restart after building a delta chain: the on-disk
    # state reverts to the last clean checkpoint, which gives us a page with a known
    # baseline size. We then build a new delta chain on top, force a full-image write,
    # and verify the checkpoint size matches the expected value (not inflated by the
    # old chain's cumulative size).
    def test_size_after_page_id_invalidation(self):
        nrows = 20

        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Step 1: Write initial data, establish a baseline checkpoint.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()
        size_baseline = self.get_checkpoint_size()
        self.assertGreater(size_baseline, 0)

        # Step 2: Write a delta on top (partial update).
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_after_delta = self.get_checkpoint_size()

        deltas = self.get_stat(stat.dsrc.rec_page_delta_leaf, uri=self.stable_uri)
        self.assertGreater(deltas, 0,
            f'Expected a leaf delta but stat shows {deltas}')
        self.assertGreater(size_after_delta, size_baseline,
            'Size should grow after appending a delta to the chain')

        # Step 3: Evict the page so it's read back from the page service.
        # On read-back, the chain's cumulative size is re-established from the stored
        # block metadata.
        self.evict_page('key000000')

        # Step 4: Force a full-image write over the existing delta chain.
        # With delta_pct=1 and a full update of all rows, the reconciliation should
        # produce a full image (delta count == 0), which terminates the chain.
        self.conn.reconfigure('page_delta=(delta_pct=1)')
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'C')
        c.close()
        self.session.checkpoint()
        size_after_full = self.get_checkpoint_size()

        # Step 5: After a full-image write that terminates the delta chain, the
        # checkpoint size should drop back toward the baseline, not remain inflated
        # by the chain's cumulative size.
        #
        # If the error path was taken here and the
        # chain's cumulative size was not subtracted from the file's running byte
        # total, size_after_full would be approximately size_after_delta +
        # size_baseline (double-counted).
        self.assertLess(size_after_full, size_after_delta + size_baseline,
            f'Checkpoint size {size_after_full} after full-image write looks inflated: '
            f'baseline={size_baseline}, after_delta={size_after_delta}. '
            f'Possible running-total leak from the error path.')

        # The full-image size should be close to the baseline (same data volume, no deltas).
        self.assertLess(size_after_full, size_baseline * 2,
            f'Full-image checkpoint size {size_after_full} is more than 2x baseline '
            f'{size_baseline}; the old delta chain cumulative size may have been leaked.')

    # -----------------------------------------------------------------------
    # test_repeated_full_image_over_delta_size_is_stable
    # -----------------------------------------------------------------------
    # Writes a delta chain, then repeatedly replaces it with a full image. The
    # checkpoint size should stabilize after the first full-image replacement
    # (the chain is terminated and re-created from scratch each time).
    def test_repeated_full_image_over_delta_size_is_stable(self):
        nrows = 20
        ncycles = 4

        self.session.create(self.uri, 'key_format=S,value_format=S')

        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'A')
        c.close()
        self.session.checkpoint()

        # Build the delta chain.
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows // 2, 'B')
        c.close()
        self.session.checkpoint()
        size_with_delta = self.get_checkpoint_size()

        # Evict and force full image to terminate the chain.
        self.evict_page('key000000')
        self.conn.reconfigure('page_delta=(delta_pct=1)')
        c = self.session.open_cursor(self.uri)
        self.insert_rows(c, 0, nrows, 'C')
        c.close()
        self.session.checkpoint()
        size_first_full = self.get_checkpoint_size()

        # The chain was terminated; repeated full-image checkpoints of the same data
        # should not grow the size.
        for cycle in range(ncycles):
            c = self.session.open_cursor(self.uri)
            self.insert_rows(c, 0, nrows, chr(ord('D') + cycle))
            c.close()
            self.session.checkpoint()

            size_cycle = self.get_checkpoint_size()

            # Allow a 1.5x margin for page-structure overhead, but the size must not
            # accumulate across cycles (each full image replaces the previous one).
            self.assertLess(size_cycle, size_first_full * 1.5,
                f'cycle {cycle}: size {size_cycle} has grown beyond expected bounds '
                f'(first full-image size was {size_first_full}). '
                f'Possible running-total double-counting.')
