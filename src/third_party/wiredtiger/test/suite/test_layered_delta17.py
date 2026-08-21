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

import wttest, wiredtiger
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_delta17.py
#
# Rebuilding a disaggregated leaf page from its base image and deltas drops every
# key whose stop is globally visible. When all of a page's keys are dropped the
# merge produces a leaf image with no entries; check the reader, verify, and
# checkpoint all tolerate that empty reconstructed page (rather than tripping the
# mutually-exclusive empty-value page flags during verification), both for a
# single-leaf tree and for one where the empty page is the leftmost of several
# children. A random cursor is checked separately: it sizes its choice of slot
# by the entry count, so no entries used to mean a divide by zero.
@disagg_test_class
class test_layered_delta17(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = test_name
    conn_base_config = ('statistics=(all),transaction_sync=(enabled,method=fsync),'
                        'page_delta=(delta_pct=100,delete_pct=100,leaf_page_delta=true),'
                        'precise_checkpoint=true,')
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 10

    # Row count and surviving tail for the multi-child tree below.
    split_uri = test_name + '_split'
    split_nitems = 400
    split_keep = 20

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    def rstat(self, key, uri):
        with wttest.open_cursor(self.session, "statistics:" + uri) as c:
            return c[key][2]

    def evict(self, key, uri):
        # Force the page holding key out of cache so the next access rebuilds it
        # from its base image and deltas.
        s = self.conn.open_session("debug=(release_evict_page)")
        c = s.open_cursor(uri, None, None)
        s.begin_transaction()
        c.set_key(key)
        c.search_near()
        c.close()
        s.rollback_transaction()
        s.close()

    def build_empty_reconstructed_page(self, uri):
        # Leave the tree with a single leaf that is only reachable by merging a
        # base image full of tombstones with leaf deltas, once every delete is
        # globally visible. On return the page is out of cache, so the next
        # access runs the merge and lands on an image with no entries.
        self.session.create(uri, "key_format=S,value_format=S,block_manager=disagg")
        value = "a" * 20

        # Populate the leaf, delete every key, and checkpoint with the oldest
        # timestamp still behind the delete. The tombstones are retained and
        # written into the leaf's base image.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.evict(str(0), uri)

        # Add and then remove a throwaway key, checkpointing each change so the
        # page carries leaf deltas on top of the tombstone-bearing base image:
        # reconstructing it now has to run the base+delta merge. The throwaway key
        # nets out to nothing, and the oldest timestamp is still behind every
        # delete so nothing is dropped at write time.
        self.session.begin_transaction()
        cursor['zzz'] = value
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(12)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(12))
        self.session.checkpoint()
        self.assertGreaterEqual(self.rstat(stat.dsrc.rec_page_delta_leaf, uri), 1)
        self.session.begin_transaction()
        cursor.set_key('zzz')
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(14)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(14))
        self.session.checkpoint()
        self.evict(str(0), uri)

        # Make every delete globally visible, then read the page back. The read
        # rebuilds it from the base image and deltas; every key is dropped and the
        # merge yields an empty leaf.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
                                ',stable_timestamp=' + self.timestamp_str(20))
        cursor.close()

    def test_empty_reconstructed_page(self):
        uri = 'table:' + self.uri
        self.build_empty_reconstructed_page(uri)

        cursor = self.session.open_cursor(uri, None, None)

        # Forward and backward scans see nothing.
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.prev(), wiredtiger.WT_NOTFOUND)

        # A point search for any prior key returns not-found rather than crashing.
        for i in range(self.nitems):
            cursor.set_key(str(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        # The empty reconstructed page verifies and checkpoints cleanly.
        self.session.verify(uri, None)
        self.session.checkpoint()

    def test_empty_reconstructed_page_random_cursor(self):
        # A random cursor picks a slot by taking the remainder of a random
        # number over the page's entry count, so an empty reconstructed page
        # divides by zero. The page has to be selected straight off its disk
        # image: with no entries there is no insert list to fall back to, and
        # the page is clean, which is the combination that sends the random
        # cursor at the disk entries a second time.
        uri = 'table:' + self.uri
        self.build_empty_reconstructed_page(uri)

        # Drop the cache so the page has to be rebuilt from the page service:
        # evicting on its own leaves the newest image a full page write. The
        # restart resets the oldest timestamp, so push it past the deletes
        # again before reading.
        self.reopen_disagg_conn(self.conn_config())
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
                                ',stable_timestamp=' + self.timestamp_str(20))

        cursor = self.session.open_cursor(uri, None, "next_random=true")
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_empty_reconstructed_leftmost_page(self):
        # Same empty reconstructed leaf, reached from a tree deep enough for the
        # internal-page key order check to run. Verify compares an internal key
        # against the largest key seen so far, but skips the 0th entry of every
        # internal page, so a single-child tree never exercises the comparison.
        # Here the leftmost leaf rebuilds empty while its siblings keep their
        # keys, leaving no largest key recorded by the time the second entry is
        # checked.
        uri = 'table:' + self.split_uri
        self.session.create(
            uri, "key_format=S,value_format=S,block_manager=disagg,leaf_page_max=4KB")
        value = "a" * 100

        # Populate enough rows to split the leaf, giving the root several
        # children.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(self.split_nitems):
            self.session.begin_transaction()
            cursor['%06d' % i] = value
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        # Confirm the checkpoint really did write the leaf as several blocks —
        # without that the tree stays one leaf deep and the internal-page check
        # below is never reached. Assert on the reconciliation counter rather than
        # btree_row_leaf: the latter is a tree-walk stat over the in-memory tree,
        # which still holds the leaf as a single page until something evicts it.
        self.assertGreaterEqual(self.rstat(stat.dsrc.rec_multiblock_leaf, uri), 1)

        # Delete every row but a trailing block. The survivors keep the last leaf
        # populated, so the root retains more than one child while the leftmost
        # leaf is left holding nothing but tombstones. The oldest timestamp stays
        # behind the deletes, so those tombstones are retained on the page.
        for i in range(self.split_nitems - self.split_keep):
            self.session.begin_transaction()
            cursor.set_key('%06d' % i)
            cursor.remove()
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.evict('%06d' % 0, uri)

        # Add and then remove a throwaway key sorting ahead of every row,
        # checkpointing each change, so the leftmost leaf carries leaf deltas on
        # top of its tombstone-bearing base image: rebuilding it now has to run
        # the base+delta merge. The throwaway key nets out to nothing.
        self.session.begin_transaction()
        cursor['!'] = value
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(12)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(12))
        self.session.checkpoint()
        self.assertGreaterEqual(self.rstat(stat.dsrc.rec_page_delta_leaf, uri), 1)
        self.session.begin_transaction()
        cursor.set_key('!')
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(14)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(14))
        self.session.checkpoint()
        cursor.close()
        self.evict('%06d' % 0, uri)

        # Make every delete globally visible. The leftmost leaf now rebuilds to
        # an image with no entries, while its siblings still hold keys.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
                                ',stable_timestamp=' + self.timestamp_str(20))

        # The surviving rows are still readable.
        cursor = self.session.open_cursor(uri, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), '%06d' % (self.split_nitems - self.split_keep))
        cursor.close()

        # Verifying the tree walks past the empty leftmost leaf into its
        # siblings.
        self.session.verify(uri, None)
        self.session.checkpoint()
