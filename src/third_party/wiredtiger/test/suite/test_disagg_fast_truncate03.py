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

import time

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat


@disagg_test_class
class test_disagg_fast_truncate03(wttest.WiredTigerTestCase):
    """
    Check that cursor traversal skips an emptied internal page in disaggregated storage.

    Fast-truncating every child of an internal page while the deletions are not yet
    globally visible can trap the page in a loop:

      1. the truncate walk empties the page and marks it for immediate eviction.
      2. eviction refuses it: the page is dirty, and dirty internal pages cannot be
         evicted in disaggregated storage.
      3. a checkpoint reconciles it, writing proxy cells for the deleted children
         (removal is deferred until the deletions are globally visible), leaving it clean.
      4. now clean, the page is evicted -- but only to WT_REF_DISK: the reference and
         the on-disk page survive.
      5. without an internal-page skip, the next walk reads the page back from the page
         service and the deleted-address cells can re-dirty it on arrival.

    The parent address aggregate records that the subtree is deleted. Once the internal
    page is on disk, cursor traversal can use that aggregate to skip the whole subtree.
    """

    uri = "table:test_disagg_fast_truncate03"
    nrows = 1000
    value = "a" * 50
    trunc_start = 100
    trunc_stop = 900

    truncate_ts = 20
    read_ts = 25
    visible_ts = 30

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return ('cache_size=50MB,statistics=(all),precise_checkpoint=true,'
                'disaggregated=(role="leader"),')

    def read_stat(self, stat_key):
        # Read with statistics=(fast): the default (all) does a full tree walk that
        # reads every fast-deleted leaf back into cache, perturbing the very loop
        # this test observes.
        with wttest.open_cursor(
            self.session, "statistics:" + self.uri, config="statistics=(fast)"
        ) as stat_cursor:
            return stat_cursor[stat_key][2]

    def retry_for_stat_increase(self, action, stat_key, baseline, msg, timeout=30):
        """Repeat an action until a counter it drives rises above baseline."""
        deadline = time.time() + timeout
        while True:
            action()
            value = self.read_stat(stat_key)
            if value > baseline:
                return value
            self.assertLess(time.time(), deadline,
                f"{msg} (still {value} after {timeout} seconds)")
            time.sleep(0.1)

    def snapshot_stats(self):
        """Snapshot the counters that witness each step of the scenario."""
        return {
            "fast_deleted": self.read_stat(stat.dsrc.rec_page_delete_fast),
            "evict_blocked": self.read_stat(
                stat.dsrc.cache_eviction_blocked_disagg_dirty_internal_page
            ),
            "internal_evicted": self.read_stat(stat.dsrc.cache_eviction_internal),
            "internal_read": self.read_stat(stat.dsrc.cache_read_internal),
            "internal_skip": self.read_stat(stat.dsrc.cursor_tree_walk_del_internal_page_skip),
        }

    def evict_keys(self, keys):
        """Force-evict the pages holding the given keys."""
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
        """Create the table, load it, checkpoint, and evict all leaves to disk."""
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))

        # Small pages give a 3-level tree (a root over ~10 internal pages over ~200
        # leaves) where the truncated range covers every child of several non-root
        # internal pages.
        self.session.create(
            self.uri,
            "key_format=i,value_format=S,block_manager=disagg,log=(enabled=false),"
            "allocation_size=512,leaf_page_max=512,internal_page_max=512,"
            "memory_page_max=4096",
        )
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=10),
        ):
            for key in range(1, self.nrows + 1):
                cursor[key] = self.value

        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(10))
        self.session.checkpoint()
        # On-disk leaves satisfy fast-delete eligibility.
        self.evict_keys(range(1, self.nrows + 1))

    def fast_truncate(self):
        """Fast-truncate the configured key range at truncate_ts."""
        with (
            wttest.open_cursor(self.session, self.uri) as start_cursor,
            wttest.open_cursor(self.session, self.uri) as stop_cursor,
            self.transaction(commit_timestamp=self.truncate_ts),
        ):
            start_cursor.set_key(self.trunc_start)
            stop_cursor.set_key(self.trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)

    def scan_table(self, read_ts=None):
        """
        Walk the whole tree left to right, the shape of a removal thread's positioning
        scan. The deletions are visible at read_ts, so the walk skips the deleted
        children, sees the internal page as empty, and marks it for eviction on ascent.
        Releasing the page then attempts that eviction inline.
        """
        if read_ts is None:
            read_ts = self.read_ts
        count = 0
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(read_ts))
        with wttest.open_cursor(self.session, self.uri) as cursor:
            while cursor.next() == 0:
                count += 1
        self.session.rollback_transaction()
        return count

    def test_skip_emptied_internal_page(self):
        self.populate()
        surviving = self.nrows - (self.trunc_stop - self.trunc_start + 1)

        # Step 1 -- the truncate walk empties the internal page. With oldest pinned at 1
        # the deletions are committed but not globally visible.
        before = self.snapshot_stats()
        self.fast_truncate()
        after = self.snapshot_stats()
        self.assertGreater(
            after["fast_deleted"], before["fast_deleted"],
            "step 1: fast truncate did not delete any pages",
        )
        self.prout("step 1: fast truncate deleted pages")

        # Step 2 -- the page is dirty, so the walk-triggered eviction is refused by the
        # disaggregated-storage guard. (The truncate walk itself may already have been
        # refused; a scan makes the refusal deterministic.)
        before = self.snapshot_stats()
        self.assertEqual(self.scan_table(), surviving)
        after = self.snapshot_stats()
        self.assertGreater(
            after["evict_blocked"], before["evict_blocked"],
            "step 2: eviction of the dirty emptied internal page was not refused",
        )
        self.assertEqual(
            after["internal_evicted"], before["internal_evicted"],
            "step 2: an internal page was evicted while dirty",
        )
        self.prout("step 2: eviction of the dirty emptied internal page was refused")

        # Step 3 -- checkpoint reconciles the page: proxy cells for the deleted children,
        # no removal, and the page is left clean. Snapshot before the checkpoint: the page
        # becomes evictable the moment it is clean, so a sample taken afterwards races with
        # the eviction server.
        before = self.snapshot_stats()
        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(self.truncate_ts))
        self.session.checkpoint()
        self.prout("step 3: checkpointed")

        # Step 4 -- the page is clean and still marked for eviction, so the walk queues it for
        # urgent eviction. An attempt the walk's own hazard pointer blocks drops the queue entry
        # rather than re-queueing it, so the walk has to be repeated until an eviction lands.
        # The reference and the on-disk page survive as WT_REF_DISK.
        self.retry_for_stat_increase(
            lambda: self.assertEqual(self.scan_table(), surviving),
            stat.dsrc.cache_eviction_internal, before["internal_evicted"],
            "step 4: the clean emptied internal page was not evicted",
        )
        self.prout("step 4: the clean emptied internal page was evicted")

        # Step 5 -- the reference survives, but its address aggregate shows that the subtree is
        # deleted. A subsequent walk skips the subtree without reading and re-dirtying the page.
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=self.read_ts - 1),
        ):
            cursor[self.nrows + 1] = self.value

        before = self.snapshot_stats()
        self.assertEqual(self.scan_table(), surviving + 1)
        after = self.snapshot_stats()
        self.assertEqual(
            after["internal_read"], before["internal_read"],
            "step 5: the evicted internal page was read back",
        )
        self.assertEqual(
            after["evict_blocked"], before["evict_blocked"],
            "step 5: the evicted internal page was re-dirtied",
        )
        self.assertGreater(
            after["internal_skip"], before["internal_skip"],
            "step 5: no deleted internal page was skipped",
        )
        self.prout("step 5: the evicted internal page was skipped")

        # Exit -- make the deletions globally visible and checkpoint: reconciliation
        # removes the children for real and the loop stops. Scans no longer trip the
        # eviction guard on this subtree.
        ts = self.timestamp_str(self.visible_ts)
        self.conn.set_timestamp(f"oldest_timestamp={ts},stable_timestamp={ts}")
        self.session.checkpoint()

        before = self.snapshot_stats()
        self.assertEqual(self.scan_table(read_ts=self.visible_ts), surviving + 1)
        after = self.snapshot_stats()
        self.assertEqual(
            after["evict_blocked"], before["evict_blocked"],
            "exit: the loop should stop once the deletions are globally visible",
        )
        self.prout("exit: the loop stopped once the deletions are globally visible")


if __name__ == "__main__":
    wttest.run()
