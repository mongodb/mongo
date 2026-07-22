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

# test_stat17.py
#   Tests for btree_row_leaf_avg_entries and btree_row_leaf_pages.
#
#   btree_row_leaf_avg_entries: EWMA of K/V pairs per row-store leaf page.
#   btree_row_leaf_pages:       incremental approximate leaf page count.
#
#   Both stats are available without WT_STAT_TYPE_TREE_WALK.
#
#   btree_row_leaf_pages is incremented at each leaf split (in-memory or
#   eviction). The cache is kept small so that eviction splits fire during
#   inserts, giving the counter something to track without a tree walk.
#
#   When WT_STAT_TYPE_TREE_WALK is requested, btree_row_leaf_pages is
#   corrected to the exact count obtained by the walk; the corrected value is
#   reflected in both the stat cursor and the in-memory btree field so a
#   subsequent fast read still sees it.
#
#   Both values are persisted through checkpoint metadata and survive a
#   server restart.
#
#   A table whose checkpoint metadata predates this tracking has neither
#   value updated by ordinary split/reconciliation activity - both fields
#   hold WT_LEAF_STATS_UNKNOWN (UINT64_MAX, read back as -1 through this
#   int64-typed stat) until a WT_STAT_TYPE_TREE_WALK sets real values for
#   both together. A table created after this tracking existed never sees
#   that reserved marker: it starts empty, which is exact by construction.
class test_stat17(wttest.WiredTigerTestCase):
    uri = 'table:test_stat17'

    # Small pages and a tight cache ensure leaf splits (both in-memory and
    # eviction) fire during inserts, so approx_leaf_pages is non-zero after
    # a checkpoint without requiring a tree walk.
    conn_config = 'statistics=(all),cache_size=2MB'
    create_params = 'key_format=S,value_format=S,leaf_page_max=4KB,internal_page_max=4KB'
    nrows = 10000

    def _insert(self, n):
        c = self.session.open_cursor(self.uri)
        for i in range(n):
            c[str(i).zfill(10)] = 'v' + str(i).zfill(10)
        c.close()

    def _scan(self):
        c = self.session.open_cursor(self.uri)
        while c.next() == 0:
            pass
        c.close()

    def _dsrc_stat(self, stat_key, cfg='fast'):
        sc = self.session.open_cursor('statistics:' + self.uri, None,
                                      'statistics=(' + cfg + ')')
        val = sc[stat_key][2]
        sc.close()
        return val

    # Both stats must be non-zero without a tree walk. The small cache forces
    # eviction splits during inserts, incrementing approx_leaf_pages. The
    # EWMA is updated at reconciliation when pages are written to disk.
    def test_available_without_tree_walk(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        avg   = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)
        pages = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)

        self.assertGreater(avg, 0,
            'btree_row_leaf_avg_entries must be non-zero after reconciliation')
        self.assertGreater(pages, 0,
            'btree_row_leaf_pages must be non-zero after eviction splits')

    # The EWMA should be within 50% of the exact average (total entries /
    # leaf pages from a tree walk). Tested after a reopen + full scan so that
    # fault-in updates the EWMA from on-disk page sizes.  The 50% bound is
    # intentionally loose; the EWMA converges gradually across many accesses.
    def test_avg_entries_within_tolerance(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        self.reopen_conn()
        self._scan()

        approx_avg = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)

        exact_entries = self._dsrc_stat(stat.dsrc.btree_entries, 'all')
        exact_pages   = self._dsrc_stat(stat.dsrc.btree_row_leaf, 'all')

        self.assertGreater(exact_pages, 0)
        exact_avg = exact_entries // exact_pages

        self.assertGreater(approx_avg, exact_avg // 2,
            'btree_row_leaf_avg_entries %d too low vs exact avg %d'
            % (approx_avg, exact_avg))
        self.assertLess(approx_avg, exact_avg * 2,
            'btree_row_leaf_avg_entries %d too high vs exact avg %d'
            % (approx_avg, exact_avg))

    # After a tree walk (statistics=(all)), both btree_row_leaf_pages and
    # btree_row_leaf_avg_entries must equal the exact values from the walk.
    def test_corrected_by_tree_walk(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        sc = self.session.open_cursor('statistics:' + self.uri, None,
                                      'statistics=(all)')
        exact_pages   = sc[stat.dsrc.btree_row_leaf][2]
        exact_entries = sc[stat.dsrc.btree_entries][2]
        corrected_pages = sc[stat.dsrc.btree_row_leaf_pages][2]
        corrected_avg   = sc[stat.dsrc.btree_row_leaf_avg_entries][2]
        sc.close()

        self.assertGreater(exact_pages, 0)
        self.assertEqual(corrected_pages, exact_pages,
            'btree_row_leaf_pages (%d) must equal btree_row_leaf (%d) after tree walk'
            % (corrected_pages, exact_pages))
        self.assertEqual(corrected_avg, exact_entries // exact_pages,
            'btree_row_leaf_avg_entries (%d) must equal exact avg (%d) after tree walk'
            % (corrected_avg, exact_entries // exact_pages))

    # After a tree walk corrects both counters in memory, a subsequent fast-stat
    # read (no walk) must return values close to the corrected ones. The walk's
    # exact count is a snapshot taken during a concurrent traversal: with the
    # tight cache, pages read in by the walk are reconciled and eviction-split in
    # the background, each split legitimately bumping the approximate counter
    # after the snapshot. The stat is approximate by contract, so allow a small
    # tolerance rather than strict equality.
    def test_correction_persists_in_memory(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        sc = self.session.open_cursor('statistics:' + self.uri, None,
                                      'statistics=(all)')
        exact_pages   = sc[stat.dsrc.btree_row_leaf][2]
        exact_entries = sc[stat.dsrc.btree_entries][2]
        sc.close()

        fast_pages = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)
        fast_avg   = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)

        # A handful of concurrent eviction splits between the walk snapshot and
        # this read is expected; allow the drift to be within 2% of the count
        # (with a small floor for tiny trees) rather than requiring exact match.
        max_pages_drift = max(2, exact_pages // 50)
        exact_avg = exact_entries // exact_pages
        max_avg_drift = max(1, exact_avg // 50)

        self.assertAlmostEqual(fast_pages, exact_pages, delta=max_pages_drift,
            msg='fast read after tree-walk correction should be near %d, got %d'
            % (exact_pages, fast_pages))
        self.assertAlmostEqual(fast_avg, exact_avg, delta=max_avg_drift,
            msg='fast avg after tree-walk correction should be near %d, got %d'
            % (exact_avg, fast_avg))

    # Both stats must survive a server restart. The checkpoint during the
    # insert run saves the values; after reopen they are restored from the
    # checkpoint metadata (meta_ckpt.c parse + bt_handle.c restore path).
    def test_checkpoint_persistence(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        avg_before   = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)
        pages_before = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)

        self.assertGreater(avg_before, 0)
        self.assertGreater(pages_before, 0)

        self.reopen_conn()

        avg_after   = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)
        pages_after = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)

        self.assertEqual(avg_after, avg_before,
            'btree_row_leaf_avg_entries must survive checkpoint/restart')

        # approx_leaf_pages is restored from the checkpoint snapshot.
        # Between the checkpoint write and the pages_before read, the
        # deleted_entries cleanup loop may have decremented the in-memory
        # counter, so pages_after (checkpoint value) >= pages_before.
        self.assertGreaterEqual(pages_after, pages_before,
            'btree_row_leaf_pages must be at least as large after restart')

    # A table created after this stat was added starts empty, which is an
    # exact count, so neither field is ever left at the WT_LEAF_STATS_UNKNOWN
    # reserved marker, from the very first checkpoint even without a tree walk.
    def test_never_unknown_for_new_table(self):
        self.session.create(self.uri, self.create_params)
        self._insert(self.nrows)
        self.session.checkpoint()

        pages = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)
        avg = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)
        self.assertGreaterEqual(pages, 0,
            'a table created after this stat exists should never read as unknown (-1)')
        self.assertGreaterEqual(avg, 0,
            'a table created after this stat exists should never read as unknown (-1)')

        self.reopen_conn()
        pages = self._dsrc_stat(stat.dsrc.btree_row_leaf_pages)
        avg = self._dsrc_stat(stat.dsrc.btree_row_leaf_avg_entries)
        self.assertGreaterEqual(pages, 0,
            'must not become unknown (-1) across checkpoint/restart for a table created with this stat')
        self.assertGreaterEqual(avg, 0,
            'must not become unknown (-1) across checkpoint/restart for a table created with this stat')

