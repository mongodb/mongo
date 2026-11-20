#!/usr/bin/env python3
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
from helper_disagg import disagg_test_class, gen_disagg_storages
from test_cc01 import test_cc_base
from wtscenario import make_scenarios
from wiredtiger import stat


class checkpoint_cleanup_resolved_base(test_cc_base):
    """
    Shared setup for checking how checkpoint cleanup treats deleted child references it has
    already resolved.

    Once a fast-truncate is globally visible, checkpoint cleanup dirties the parent internal
    page and the next reconciliation discards the child's block and its page-deleted
    information, writing no cell for it. The reference itself stays in the parent's in-memory
    page index in the deleted state until the parent is evicted or split. Later cleanup passes
    must treat those references as done rather than counting them as removed again and
    re-dirtying the parent. In disaggregated storage a dirty internal page cannot be evicted, so
    re-dirtying it on every pass keeps it, and the references under it, in memory indefinitely.
    """

    uri = "table:cc12"
    nrows = 1000
    value = "a" * 50
    # The range ends partway through the key space so the internal pages around its end keep
    # live children and are not reconciled to empty.
    trunc_start = 100
    trunc_stop = 550

    truncate_ts = 20
    visible_ts = 30
    settle_rounds = 3
    idle_rounds = 3

    def conn_config(self):
        config = ('cache_size=50MB,statistics=(all),precise_checkpoint=true,'
                  'checkpoint_cleanup=(wait=100000),')
        if self.is_disagg_scenario():
            config += 'disaggregated=(role="leader"),'
        return config

    def read_stat(self, stat_key):
        # Use statistics=(fast): the default (all) walks the tree and reads pages into cache.
        with wttest.open_cursor(
            self.session, "statistics:" + self.uri, config="statistics=(fast)"
        ) as stat_cursor:
            return stat_cursor[stat_key][2]

    def snapshot_stats(self):
        return {
            "cleanup_removed": self.read_stat(stat.dsrc.checkpoint_cleanup_pages_removed),
            "real_deleted": self.read_stat(stat.dsrc.rec_page_delete),
            "internal_read": self.read_stat(stat.dsrc.cache_read_internal),
            "internal_evicted": self.read_stat(stat.dsrc.cache_eviction_internal),
        }

    def evict_keys(self, keys):
        with (
            wttest.open_cursor(
                self.session, self.uri, config="debug=(release_evict)"
            ) as evict_cursor,
            self.transaction(rollback=True),
        ):
            for key in keys:
                evict_cursor.set_key(key)
                evict_cursor.search()
                evict_cursor.reset()

    def populate(self):
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))
        # Small pages give a tree with several leaves under each internal page.
        block_manager = "block_manager=disagg," if self.is_disagg_scenario() else ""
        self.session.create(
            self.uri,
            "key_format=i,value_format=S," + block_manager + "log=(enabled=false),"
            "allocation_size=512,leaf_page_max=512,internal_page_max=512,"
            "memory_page_max=4096",
        )
        # Load in batches, advancing stable after each one so that precise checkpoint lets
        # eviction write the loaded pages. Commit timestamps stay below truncate_ts.
        batch = 5000
        for i, first in enumerate(range(1, self.nrows + 1, batch)):
            commit_ts = 10 + i
            self.assertLess(commit_ts, self.truncate_ts)
            with (
                wttest.open_cursor(self.session, self.uri) as cursor,
                self.transaction(commit_timestamp=commit_ts),
            ):
                for key in range(first, min(first + batch, self.nrows + 1)):
                    cursor[key] = self.value
            self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(commit_ts))

        self.session.checkpoint()
        # On-disk leaves are eligible for fast-delete.
        self.evict_keys(range(1, self.nrows + 1))

    def fast_truncate(self):
        with (
            wttest.open_cursor(self.session, self.uri) as start_cursor,
            wttest.open_cursor(self.session, self.uri) as stop_cursor,
            self.transaction(commit_timestamp=self.truncate_ts),
        ):
            start_cursor.set_key(self.trunc_start)
            stop_cursor.set_key(self.trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)

    def truncate_and_make_visible(self):
        """Fast-truncate the range, then make the truncate globally visible."""
        self.populate()
        self.fast_truncate()
        self.assertGreater(
            self.read_stat(stat.dsrc.cursor_truncate_fast_delete_pages), 0,
            "fast truncate did not delete any pages",
        )
        ts = self.timestamp_str(self.visible_ts)
        self.conn.set_timestamp(f"oldest_timestamp={ts},stable_timestamp={ts}")

    def run_rounds(self, count):
        """
        Run rounds of one cleanup pass followed by one checkpoint that reconciles whatever the
        pass dirtied, and return the per-round deltas.
        """
        rounds = []
        for _ in range(count):
            before = self.snapshot_stats()
            self.wait_for_cc_to_run()
            after_cleanup = self.snapshot_stats()
            self.session.checkpoint()
            after = self.snapshot_stats()
            rounds.append({
                "removed": after_cleanup["cleanup_removed"] - before["cleanup_removed"],
                "rec_page_delete": after["real_deleted"] - before["real_deleted"],
                "internal_read": after["internal_read"] - before["internal_read"],
                "internal_evicted": after["internal_evicted"] - before["internal_evicted"],
            })
        return rounds

    def check_resolved_refs_not_reprocessed(self):
        # The first rounds reclaim the deleted pages; after that nothing in the tree changes.
        self.truncate_and_make_visible()
        rounds = self.run_rounds(self.settle_rounds + self.idle_rounds)
        self.pr(f"rounds: {rounds}")
        # Once reclamation is done, a cleanup pass must not count a resolved reference as
        # removed again, and must not dirty a parent that the next checkpoint then reconciles
        # to empty again.
        idle = rounds[self.settle_rounds:]
        self.assertTrue(
            all(r["removed"] == 0 and r["rec_page_delete"] == 0 for r in idle),
            f"idle cleanup passes keep reprocessing resolved deleted references: {rounds}",
        )


@disagg_test_class
class test_cc12(checkpoint_cleanup_resolved_base):
    """
    Check that checkpoint cleanup does not keep reprocessing deleted child references it has
    already resolved, in a three-level tree.
    """

    disagg_storages = gen_disagg_storages()
    scenarios = make_scenarios(disagg_storages)

    def test_resolved_deleted_refs_not_reprocessed(self):
        self.check_resolved_refs_not_reprocessed()

    def test_cursor_freed_page_del_still_reclaimed(self):
        """
        A cursor walk frees the fast-delete information of a globally visible deleted ref
        without marking its parent dirty, and the page is still referenced by the parent's
        last written image. Checkpoint cleanup must still process such a ref.
        """
        self.populate()
        self.fast_truncate()
        fast_deleted = self.read_stat(stat.dsrc.cursor_truncate_fast_delete_pages)
        self.assertGreater(fast_deleted, 0, "fast truncate did not delete any pages")

        # Write the parents with the fast-delete information while the truncate is not yet
        # globally visible, leaving them clean and in memory.
        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(self.truncate_ts))
        self.session.checkpoint()

        # Make the truncate globally visible, then walk the table: the walk frees the
        # fast-delete information of the deleted refs.
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(self.truncate_ts))
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(rollback=True),
        ):
            while cursor.next() == 0:
                pass

        rounds = self.run_rounds(1)
        self.pr(f"rounds: {rounds}")
        self.assertGreaterEqual(rounds[0]["removed"], fast_deleted,
            f"cleanup did not process the deleted refs: {rounds}")


@disagg_test_class
class test_cc12_four_levels(checkpoint_cleanup_resolved_base):
    """
    Same check in a four-level tree, so that the grandparent of the deleted leaves is not the
    root. The truncate covers several whole internal pages above the leaves, but no whole page
    one level up.
    """

    nrows = 50000
    trunc_start = 3100
    trunc_stop = 5300

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def test_resolved_deleted_refs_not_reprocessed(self):
        self.check_resolved_refs_not_reprocessed()
        self.assertEqual(self.read_stat(stat.dsrc.btree_maximum_depth), 4,
            "the tree is not four levels deep")


if __name__ == '__main__':
    wttest.run()
