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

# test_size_stats01.py
# A b-tree size summary accumulated into the data-source statistics as a cursor traverses the tree
# (the debug=(size_stats) cursor flag), then read back with a statistics cursor. The traversal
# pattern matches the ordered forward scan MongoDB's collHash performs, so the accounting is a free
# side-effect of a scan the caller already does. The summary is raw constituents only (per-page-type
# counts and bytes, leaf key/value bytes and counts, and a leaf page-size histogram); derived figures
# such as overhead are computed by this test, not the engine.
#
# The summary also works on layered (disaggregated) tables, where it measures the stable
# constituent's on-disk row-store btree; running under the disagg hook exercises that path.
class test_size_stats01(wttest.WiredTigerTestCase):
    uri = 'table:test_size_stats01'
    params = 'key_format=S,value_format=S,leaf_page_max=32KB,internal_page_max=16KB'
    nrecords = 10000

    # Read the size summary for a btree, driving the traversal the way the target consumer (collHash)
    # scans its record store, so the accounting is a free side-effect of that scan:
    #
    #   1. one next() on a fresh cursor to discover the first key. This walks from the root,
    #      accounting the root, the leftmost internal path and the first leaf.
    #   2. a seek back to that first key, then a next() loop to the end. The seek repositions via a
    #      search, which does not hit the walk's per-page hook, so the first leaf is not
    #      double-counted; the loop accounts every remaining leaf and internal page.
    #
    # Neither step is an extra scan. The statistics are then read with a statistics=(fast) cursor that
    # does not re-walk the tree. Overhead and the uncompressed total are derived here, mirroring the
    # analysis the engine leaves to the consumer.
    #
    # Reopen first so every page image is loaded from the last checkpoint. Size_stats counts cells in
    # page->dsk; an in-cache page can keep a pre-delete image while the cursor skips tombstones, so
    # without a reopen the summary and the scan disagree.
    def size_summary(self, uri=None, enable=True):
        if uri is None:
            uri = self.uri
        self.reopen_conn(config='statistics=(fast)')
        session = self.session

        config = 'debug=(size_stats)' if enable else None
        cursor = session.open_cursor(uri, None, config)
        scanned = 0
        if cursor.next() == 0:
            # The probe discovered the first key; seek back to it and traverse forward, reusing
            # one cursor rather than scanning a second time.
            first_key = cursor.get_key()
            cursor.set_key(first_key)
            self.assertEqual(cursor.search(), 0)
            scanned = 1
            while cursor.next() == 0:
                scanned += 1
        cursor.close()

        statc = session.open_cursor('statistics:' + uri, None, 'statistics=(fast)')
        def g(key):
            return statc[key][2]
        s = dict(
            leaf=g(stat.dsrc.btree_size_leaf_pages),
            internal=g(stat.dsrc.btree_size_internal_pages),
            overflow=g(stat.dsrc.btree_size_overflow_pages),
            leaf_bytes=g(stat.dsrc.btree_size_leaf_bytes),
            internal_bytes=g(stat.dsrc.btree_size_internal_bytes),
            overflow_bytes=g(stat.dsrc.btree_size_overflow_bytes),
            key=g(stat.dsrc.btree_size_key_bytes),
            value=g(stat.dsrc.btree_size_value_bytes),
            key_count=g(stat.dsrc.btree_size_key_count),
            value_count=g(stat.dsrc.btree_size_value_count),
            maxleaf=g(stat.dsrc.btree_maxleafpage),
        )
        s['hist'] = [g(getattr(stat.dsrc, 'btree_size_leaf_hist_%d' % i)) for i in range(9)]
        statc.close()

        s['scanned'] = scanned
        s['total'] = s['leaf_bytes'] + s['internal_bytes'] + s['overflow_bytes']
        s['overhead'] = s['total'] - (s['key'] + s['value'])
        return s

    # Build a tree spanning many leaf pages, then delete most of the keys so the pages are left
    # underfull (WiredTiger does not merge pages back together). Every keep_mod-th key survives, so
    # keep_mod=10 deletes ~90% and keep_mod=100 deletes ~99%. Leave the connection open; size_summary
    # reopens to load post-checkpoint page images.
    def populate_underfull(self, keep_mod=10, nrecords=None):
        if nrecords is None:
            nrecords = self.nrecords
        self.close_conn()
        self.open_conn()
        self.session.create(self.uri, self.params)

        cursor = self.session.open_cursor(self.uri)
        value = 'v' * 100
        for i in range(nrecords):
            cursor['key%08d' % i] = value
        cursor.close()
        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri)
        for i in range(nrecords):
            if i % keep_mod != 0:
                cursor.set_key('key%08d' % i)
                self.assertEqual(cursor.remove(), 0)
        cursor.close()
        self.session.checkpoint()

    # Populate with large values that exceed the leaf-value limit, forcing overflow pages.
    def populate_overflow(self, nrecords, valuesize):
        self.close_conn()
        self.open_conn()
        self.session.create(self.uri,
          'key_format=S,value_format=S,leaf_page_max=4KB,internal_page_max=4KB')
        cursor = self.session.open_cursor(self.uri)
        value = 'v' * valuesize
        for i in range(nrecords):
            cursor['key%08d' % i] = value
        cursor.close()
        self.session.checkpoint()

    # Append-only workload: a single btree filled with monotonically increasing keys, never updated
    # or deleted. Leaf pages pack tightly, the opposite end of the spectrum from the underfull tree.
    def populate_append(self, nrecords, valuesize):
        self.close_conn()
        self.open_conn(config='cache_size=2GB')
        self.session.create(self.uri,
          'key_format=Q,value_format=u,leaf_page_max=32KB,internal_page_max=16KB')
        cursor = self.session.open_cursor(self.uri)
        value = b'\xa5' * valuesize
        for i in range(nrecords):
            cursor[i + 1] = value
        cursor.close()
        self.session.checkpoint()

    def test_size_metrics(self):
        self.populate_underfull()

        s = self.size_summary()

        # After reopen, the scan and size_stats see the same post-checkpoint page images: every live
        # record has one on-page key and one on-page value.
        self.assertGreater(s['key_count'], 0)
        self.assertEqual(s['key_count'], s['value_count'])
        self.assertEqual(s['scanned'], s['key_count'])

        # Page counts are populated: leaf and internal pages exist (the root is an internal page), no
        # overflow for these small values.
        self.assertGreater(s['leaf'], 0)
        self.assertGreater(s['internal'], 0)
        self.assertEqual(s['overflow'], 0)

        # Byte accounting is self-consistent: derived overhead is non-negative, and the uncompressed
        # total is at least the counted user data.
        self.assertGreaterEqual(s['overhead'], 0)
        self.assertGreaterEqual(s['total'], s['key'] + s['value'])

        # The histogram covers every leaf page exactly once.
        self.assertEqual(sum(s['hist']), s['leaf'])
        self.assertEqual(s['maxleaf'], 32 * 1024)

        # Without the flag, the scan does not touch the size statistics: they stay zero.
        s0 = self.size_summary(enable=False)
        self.assertEqual(s0['leaf'], 0)
        self.assertEqual(s0['internal'], 0)
        self.assertEqual(s0['key_count'], 0)
        self.assertEqual(sum(s0['hist']), 0)

    # Delete 99% of the keys, leaving a large population of near-empty leaf pages that WiredTiger
    # never merges back together. This is the pathology the page-size histogram exists to surface:
    # the pages are uniformly underfull, concentrated in the smallest bucket.
    def test_size_underfull(self):
        self.populate_underfull(keep_mod=100, nrecords=200000)

        s = self.size_summary()

        # Byte accounting stays self-consistent regardless of tree shape.
        self.assertGreaterEqual(s['overhead'], 0)

        # The tree is built from many leaf pages, and after the deletes they are heavily underfull.
        self.assertGreater(s['leaf'], 50)

        # The underfull pages are a uniform population, not a few outliers: the smallest bucket
        # (<1/8 of leaf page max) holds the vast majority, and the buckets sum to the leaf page count.
        self.assertEqual(sum(s['hist']), s['leaf'])
        self.assertGreaterEqual(s['hist'][0], int(0.8 * s['leaf']))

    # An overflow item's payload is counted against value bytes (via the overflow page), so it is not
    # mistaken for overhead, keeping derived overhead a small fraction of a well-packed tree.
    #
    # Skipped under the disagg hook: disaggregated storage's larger page sizing keeps these values
    # on-page, so the stable constituent never forms the overflow pages this test needs.
    @wttest.skip_for_hook("disagg", "cannot force overflow pages on the layered stable constituent")
    def test_size_overflow(self):
        nrecords, valuesize = 500, 2000
        self.populate_overflow(nrecords, valuesize)

        s = self.size_summary()

        # Large values must have produced overflow pages.
        self.assertGreater(s['overflow'], 0)
        self.assertGreater(s['overflow_bytes'], 0)
        # The overflow payloads (valuesize + NUL for the S format) dominate user value bytes; if they
        # were miscounted as overhead, derived overhead would balloon past the user data.
        self.assertGreaterEqual(s['value'], nrecords * valuesize)
        self.assertGreaterEqual(s['overhead'], 0)
        self.assertLess(s['overhead'], s['key'] + s['value'])
        # Each record contributes one key and one value, including overflow values.
        self.assertEqual(s['value_count'], nrecords)
        self.assertEqual(s['key_count'], nrecords)

    # A ~5GB single btree from an append-only workload. 4KB values keep every record on-page (no
    # overflow) while the tree spans hundreds of thousands of leaf pages, so this exercises the size
    # summary at scale and at the low-overhead end of the spectrum.
    @wttest.longtest('append-only workload building a ~5GB single-btree database')
    def test_size_append_5gb(self):
        valuesize = 4096
        nrecords = (5 * 1024 * 1024 * 1024) // valuesize

        self.populate_append(nrecords, valuesize)

        s = self.size_summary()

        # Overhead (derived) is non-negative and a small fraction of well-packed user data.
        self.assertGreaterEqual(s['overhead'], 0)

        # A single append-only btree at this scale: many leaf pages, no overflow, multiple GB. The
        # 'u' value format stores exactly valuesize bytes per record (no trailing NUL).
        self.assertGreater(s['leaf'], 1000)
        self.assertEqual(s['overflow'], 0)
        self.assertGreaterEqual(s['total'], 4 * 1024 * 1024 * 1024)
        self.assertGreaterEqual(s['value'], nrecords * valuesize)
        self.assertEqual(s['key_count'], s['value_count'])

        # Append-only leaves sit well above the underfull regime, and overhead is a small fraction of
        # user data. The exact split fill-factor is an implementation detail (append leaves a bimodal
        # fill, not uniformly full pages), so assert the mean leaf fill rather than a specific
        # histogram bucket, and assert the smallest bucket is not where the weight lands.
        self.assertEqual(sum(s['hist']), s['leaf'])
        mean_leaf_fill = s['leaf_bytes'] / s['leaf']
        self.assertGreater(mean_leaf_fill, 0.375 * s['maxleaf'])
        self.assertLess(s['hist'][0], s['leaf'] // 2)
        self.assertLess(s['overhead'] * 100 // (s['key'] + s['value']), 25)
