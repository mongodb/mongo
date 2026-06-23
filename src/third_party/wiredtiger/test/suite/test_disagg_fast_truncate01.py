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
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat


@disagg_test_class
class test_disagg_fast_truncate01(wttest.WiredTigerTestCase):
    """
    A parent split must not discard a fast-truncated child's block while a checkpoint that
    references the child is in flight: disagg discards are immediate and irreversible. This
    situation will result in a completed checkpoint carrying a proxy cell for a page already
    discarded in PALite. The consequence is a double discard.

    The test reconstructs this situation and exposes the problem by reloading the checkpoint
    and running verify.
    """

    uri = "table:test_disagg_fast_truncate01"
    nrows = 200
    value = "a" * 50
    big_value = "b" * 400
    trunc_start = 50
    trunc_stop = 190

    # Cumulative appends of big_value (30 * 400 B = 12 kB) well exceed memory_page_max (4096),
    # arming the in-memory split of the rightmost leaf.
    append_count = 30

    truncate_ts = 20
    visible_ts = 30

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return 'cache_size=50MB,statistics=(all),disaggregated=(role="leader"),'

    def read_stat(self, stat_key, uri=""):
        """Return the current value of stat_key. Omit the URI for connection-level stats."""
        with wttest.open_cursor(self.session, "statistics:" + uri) as stat_cursor:
            return stat_cursor[stat_key][2]

    def _run_checkpoint(self):
        session = self.conn.open_session()
        session.checkpoint()
        session.close()

    def _wait_for_processed_table(self, last_ckpt_gen):
        for _ in range(300):
            ckpt_gen = self.read_stat(stat.dsrc.btree_checkpoint_generation, self.uri)
            if ckpt_gen > last_ckpt_gen:
                return
            time.sleep(0.1)
        self.fail("the checkpoint did not process the table")

    @contextmanager
    def checkpoint_in_background(self):
        """
        Run a checkpoint in a background thread and yield once btree_checkpoint_generation
        advances (confirming the table's image is sealed). The thread must complete before
        the connection closes.
        """
        # The history-store delay holds this checkpoint open for 10 seconds after the
        # data trees have synced.
        self.conn.reconfigure("timing_stress_for_test=[history_store_checkpoint_delay]")
        last_ckpt_gen = self.read_stat(stat.dsrc.btree_checkpoint_generation, self.uri)

        with ThreadPoolExecutor(max_workers=1) as executor:
            checkpoint = executor.submit(self._run_checkpoint)
            self._wait_for_processed_table(last_ckpt_gen)
            try:
                yield
                self.assertFalse(
                    checkpoint.done(),
                    "the checkpoint window closed before the body finished",
                )
            finally:
                self.conn.reconfigure("timing_stress_for_test=[]")
                checkpoint.result()

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

        # Small pages give a 3-level tree where the truncated leaves and the append point
        # share a non-root parent; the small memory_page_max arms the in-memory split path.
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

    def split_parent(self):
        """Trigger an in-memory split that reaps the globally-visible deleted refs."""
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=self.visible_ts + 5),
        ):
            for key in range(self.nrows + 1, self.nrows + self.append_count + 1):
                cursor[key] = self.big_value

        self.evict_keys([self.nrows + 1])

    def test_split_reaps_deleted_child_mid_checkpoint(self):
        """A checkpoint must never both contain a discard and reference its page."""
        self.populate()

        fast_delete_before = self.read_stat(stat.dsrc.rec_page_delete_fast, self.uri)
        inmem_split_before = self.read_stat(stat.dsrc.cache_inmem_split, self.uri)

        # With oldest pinned at 1 the truncate is committed but not globally visible, so
        # the next reconciliation of the parent keeps proxy cells for the leaves.
        self.fast_truncate()
        self.conn.set_timestamp(
            "stable_timestamp=" + self.timestamp_str(self.truncate_ts)
        )

        # Inside the window: truncate becomes globally visible, split reaps the deleted
        # children while the checkpoint that sealed their proxy cells is still open.
        with self.checkpoint_in_background():
            ts = self.timestamp_str(self.visible_ts)
            self.conn.set_timestamp(f"oldest_timestamp={ts},stable_timestamp={ts}")
            self.split_parent()

        self.assertGreater(
            self.read_stat(stat.dsrc.rec_page_delete_fast, self.uri),
            fast_delete_before,
            "fast truncate did not trigger",
        )

        self.assertGreater(
            self.read_stat(stat.dsrc.cache_inmem_split, self.uri),
            inmem_split_before,
            "the append leaf did not split in memory",
        )

        # The completed checkpoint must not reference a discarded page.
        self.reopen_disagg_conn(self.conn_config())
        self.session.verify(self.uri, None)


if __name__ == "__main__":
    wttest.run()
