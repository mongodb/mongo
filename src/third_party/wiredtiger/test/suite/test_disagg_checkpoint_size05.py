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

import re, wttest
from wiredtiger import stat
from helper import simulate_crash_restart
from helper_disagg import disagg_test_class

# Test that block_size statistic reflects the checkpoint size for disaggregated storage.
# For disaggregated tables there is no underlying file, so block_size is sourced from the
# most recent checkpoint size. The stat is updated via two code paths that are both exercised
# here:
#
#     Slow path  -- statistics=(all) opens the dhandle and reads the checkpoint size from the
#                   block manager handle, which is updated at every checkpoint.
#     Fast path  -- statistics=(size) avoids opening the dhandle and reads the size directly
#                   from the checkpoint entry in the file's metadata.
#
# At startup, the block manager handle is initialized from the checkpoint metadata so the
# slow-path stat is also correct before the first new checkpoint is taken after a restart.
@disagg_test_class
class test_disagg_checkpoint_size05(wttest.WiredTigerTestCase):

    uri_base = "test_disagg_ckpt_size05"
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'
    uri = "layered:" + uri_base
    stable_uri = "file:" + uri_base + ".wt_stable"

    def insert_rows(self, n, value='x', start=0, uri=None):
        cursor = self.session.open_cursor(uri or self.uri)
        for i in range(start, start + n):
            cursor[f'key{i:08d}'] = value * 100
        cursor.close()

    # Read block_size from a statistics cursor using the slow path that opens the dhandle.
    def get_block_size_slow(self):
        cstat = self.session.open_cursor('statistics:' + self.stable_uri, None, 'statistics=(all)')
        sz = cstat[stat.dsrc.block_size][2]
        cstat.close()
        return sz

    # Read block_size via statistics=(size) using the fast path that reads directly from metadata.
    def get_block_size_fast(self):
        cstat = self.session.open_cursor('statistics:' + self.stable_uri, None, 'statistics=(size)')
        sz = cstat[stat.dsrc.block_size][2]
        cstat.close()
        return sz

    # Read the checkpoint size out of the raw metadata string (ground truth).
    def get_ckpt_size_from_meta(self):
        mc = self.session.open_cursor('metadata:')
        mc.set_key(self.stable_uri)
        self.assertEqual(mc.search(), 0)
        sizes = re.findall(r',size=(\d+),', mc.get_value())
        mc.close()
        self.assertGreater(len(sizes), 0, "No size= found in checkpoint metadata")
        return int(sizes[-1])

    # Test that querying stats before the first checkpoint returns zero and does not error.
    def test_block_size_zero_before_first_checkpoint(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(500)

        self.assertEqual(self.get_block_size_slow(), 0,
            "slow-path block_size should be zero before the first checkpoint")
        self.assertEqual(self.get_block_size_fast(), 0,
            "fast-path block_size should be zero before the first checkpoint")

    # Test that both paths must return the same value, and it must match the raw metadata.
    def test_slow_and_fast_path_agree_with_metadata(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(1000)
        self.session.checkpoint()

        meta = self.get_ckpt_size_from_meta()
        self.assertEqual(self.get_block_size_fast(), meta,
            f"statistics=(size) fast path should match metadata size ({meta})")
        self.assertEqual(self.get_block_size_slow(), meta,
            f"statistics=(all) slow path should match metadata size ({meta})")

    # Test block_size grows after additional data is inserted and a new checkpoint is taken.
    def test_block_size_increases_with_data(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(500)
        self.session.checkpoint()
        size_first = self.get_block_size_fast()

        self.insert_rows(1000, start=500)
        self.session.checkpoint()
        fast_second = self.get_block_size_fast()
        meta_second = self.get_ckpt_size_from_meta()

        self.assertGreater(fast_second, size_first,
            f"fast-path block_size should increase after inserting more data")
        self.assertEqual(fast_second, meta_second,
            f"fast path ({fast_second}) should match metadata ({meta_second}) after second checkpoint")
        self.assertEqual(self.get_block_size_slow(), meta_second,
            f"slow path should match metadata ({meta_second}) after second checkpoint")

    # Test block_size is correct immediately after restart, before any new checkpoint is taken.
    def test_block_size_after_restart(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(1000)
        self.session.checkpoint()

        size_before = self.get_block_size_fast()

        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        self.assertEqual(self.get_block_size_fast(), size_before,
            f"fast-path block_size should match the pre-restart checkpoint size")
        self.assertEqual(self.get_block_size_slow(), size_before,
            f"slow-path block_size should be restored from metadata after restart")

        # A new checkpoint after restart should keep both paths in sync.
        self.insert_rows(500, start=1000)
        self.session.checkpoint()

        fast_new = self.get_block_size_fast()
        meta_new = self.get_ckpt_size_from_meta()

        self.assertGreater(fast_new, size_before,
            "fast-path block_size should increase after inserting data and checkpointing post-restart")
        self.assertEqual(fast_new, meta_new,
            f"fast path ({fast_new}) should match metadata ({meta_new}) after post-restart checkpoint")
        self.assertEqual(self.get_block_size_slow(), meta_new,
            f"slow path should match metadata ({meta_new}) after post-restart checkpoint")

    # statistics=(size) on a layered: URI aggregates both ingest and stable block_size via the
    # slow path (opens both dhandles). Verify the result is non-zero and larger than the
    # stable-only value, since the ingest table contributes too.
    def test_block_size_layered_uri_includes_ingest(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(500)
        self.session.checkpoint()

        stable_size = self.get_block_size_slow()

        cstat = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        layered_size = cstat[stat.dsrc.block_size][2]
        cstat.close()

        self.assertGreater(layered_size, 0,
            "block_size for a layered URI should be non-zero after a checkpoint")
        self.assertGreaterEqual(layered_size, stable_size,
            f"layered block_size ({layered_size}) should be >= stable block_size ({stable_size})")

    # The fast path reads the size from the last checkpoint in the metadata. Verify that inserting
    # more data without checkpointing does not change the reported size -- it must reflect the
    # last checkpoint, not uncommitted data. Also force eviction to confirm that reconciliation
    # writing dirty pages to disk does not affect the reported size either.
    def test_block_size_unchanged_without_checkpoint(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(500)
        self.session.checkpoint()

        size_after_ckpt = self.get_block_size_fast()
        meta_after_ckpt = self.get_ckpt_size_from_meta()
        self.assertEqual(size_after_ckpt, meta_after_ckpt)

        # Insert more data but do NOT checkpoint.
        self.insert_rows(1000, start=500)

        self.assertEqual(self.get_block_size_fast(), size_after_ckpt,
            "fast-path block_size should not change without a new checkpoint")
        self.assertEqual(self.get_block_size_slow(), size_after_ckpt,
            "slow-path block_size should not change without a new checkpoint")
        self.assertEqual(self.get_block_size_slow(), self.get_block_size_fast(),
            f"slow-path block_size should still match the fast-path block_size without a new checkpoint")

        # Force eviction so reconciliation writes dirty pages to disk. Both paths read from
        # checkpoint metadata, so neither should change without a new checkpoint.
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        for i in range(0, 1500, 100):
            evict_cursor.set_key(f'key{i:08d}')
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()

        self.assertEqual(self.get_block_size_fast(), size_after_ckpt,
            "fast-path block_size should not change after eviction without a checkpoint")
        self.assertEqual(self.get_block_size_slow(), size_after_ckpt,
            "slow-path block_size should not change after eviction without a checkpoint")
        self.assertEqual(self.get_block_size_slow(), self.get_block_size_fast(),
            f"slow-path block_size should still match the fast-path block_size after eviction without a checkpoint")

    # Simulate crashes and verify block_size is always consistent with the last successful
    # checkpoint. First crash without a checkpoint (size should not change), then checkpoint and
    # crash again (size should reflect the new checkpoint).
    def test_block_size_survives_crash(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(1000)
        self.session.checkpoint()

        size_first_ckpt = self.get_block_size_fast()
        self.assertGreater(size_first_ckpt, 0)

        # Insert more data without checkpointing, then simulate a crash.
        self.insert_rows(1000, start=1000)

        with self.expectedStdoutPattern("Removing local file"):
            simulate_crash_restart(self, ".", "RESTART")

        # After crash recovery, both paths should report the pre-crash checkpoint size.
        self.assertEqual(self.get_block_size_fast(), size_first_ckpt,
            "fast-path block_size should reflect the last checkpoint after crash without new checkpoint")
        self.assertEqual(self.get_block_size_slow(), size_first_ckpt,
            "slow-path block_size should reflect the last checkpoint after crash without new checkpoint")

        # Now insert data, checkpoint, then crash again.
        self.insert_rows(500, start=1000)
        self.session.checkpoint()

        size_second_ckpt = self.get_block_size_fast()
        self.assertGreater(size_second_ckpt, size_first_ckpt)

        # Insert uncheckpointed data and crash.
        self.insert_rows(500, start=1500)

        with self.expectedStdoutPattern("Removing local file"):
            simulate_crash_restart(self, "RESTART", "RESTART2")

        # Size should reflect the second checkpoint, not the uncheckpointed data.
        self.assertEqual(self.get_block_size_fast(), size_second_ckpt,
            "fast-path block_size should reflect the second checkpoint after crash")
        self.assertEqual(self.get_block_size_slow(), size_second_ckpt,
            "slow-path block_size should reflect the second checkpoint after crash")

    # Every statistics cursor type must report the same block_size on a follower as it did on
    # the leader for the same checkpoint: the non-walk types read it from checkpoint metadata
    # directly, and the walk types get it from the checkpoint dhandle they open - both must
    # agree, not fall back to an empty-ingest base.
    def test_block_size_same_on_leader_and_follower(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_rows(1000)
        self.session.checkpoint()

        stat_configs = ('fast', 'size', 'all', 'cache_walk', 'tree_walk')

        def block_sizes():
            sizes = {}
            for cfg in stat_configs:
                cstat = self.session.open_cursor(
                    'statistics:' + self.uri, None, f'statistics=({cfg})')
                sizes[cfg] = cstat[stat.dsrc.block_size][2]
                cstat.close()
            return sizes

        leader_sizes = block_sizes()
        self.assertEqual(len(set(leader_sizes.values())), 1,
            f"leader block_size should agree across statistics types: {leader_sizes}")

        # Step down to follower - same connection, same on-disk checkpoint.
        self.conn.reconfigure('disaggregated=(role="follower")')

        follower_sizes = block_sizes()
        self.assertEqual(len(set(follower_sizes.values())), 1,
            f"follower block_size should agree across statistics types: {follower_sizes}")

        leader_size = next(iter(leader_sizes.values()))
        follower_size = next(iter(follower_sizes.values()))
        self.assertGreater(follower_size, 0,
            "follower block_size should reflect the checkpoint, not an empty base")
        self.assertEqual(follower_size, leader_size,
            f"follower block_size ({follower_size}) should match leader block_size ({leader_size})")
