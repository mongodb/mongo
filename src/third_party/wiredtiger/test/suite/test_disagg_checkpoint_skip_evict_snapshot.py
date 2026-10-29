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

import threading, time
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wtthread import checkpoint_thread
from wiredtiger import stat

# test_disagg_checkpoint_skip_evict_snapshot.py
#   Test for the precise-checkpoint optimization that lets a checkpoint skip re-reconciling
#   a page eviction already reconciled under the same checkpoint snapshot.
#
#   Eviction scrubs a dirty page and its content is always durable in the page log, so a page
#   reconciled under the checkpoint's snapshot is left dirty, durable, and stamped. When the
#   checkpoint walk reaches it, it can be skipped.
#
#   The checkpoint publishes its eviction snapshot during prepare, then stalls before walking
#   the tree. We force application-thread eviction inside that window so the pages are reconciled
#   under the checkpoint's snapshot first.
@disagg_test_class
class test_disagg_checkpoint_skip_evict_snapshot(wttest.WiredTigerTestCase):
    ntables = 20
    nrows = 500

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return 'cache_size=1GB,statistics=(all),precise_checkpoint=true,' \
            'timing_stress_for_test=[checkpoint_slow],disaggregated=(role="leader"),'

    def read_stat(self, stat_key, uri=""):
        with wttest.open_cursor(self.session, "statistics:" + uri) as stat_cursor:
            return stat_cursor[stat_key][2]

    def uri(self, t):
        return f'layered:test_disagg_ckpt_skip_{t}'

    def key(self, i):
        return f'key{i:08d}'

    def evict_all(self):
        # Force eviction of every leaf page of every table from an application thread. A session-level
        # release_evict both evicts pages as they are released and sets the flag that lets forced
        # eviction bypass the precise-checkpoint re-reconciliation gate.
        s = self.conn.open_session("debug=(release_evict_page=true)")
        s.begin_transaction()
        for t in range(self.ntables):
            c = s.open_cursor(self.uri(t), None, None)
            for i in range(1, self.nrows + 1):
                c.set_key(self.key(i))
                c.search()
                c.reset()
            c.close()
        s.rollback_transaction()
        s.close()

    def write_all(self, value, ts):
        for t in range(self.ntables):
            c = self.session.open_cursor(self.uri(t))
            for i in range(1, self.nrows + 1):
                self.session.begin_transaction()
                c[self.key(i)] = value
                self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
            c.close()

    def test_skip(self):
        for t in range(self.ntables):
            self.session.create(self.uri(t), 'key_format=S,value_format=S')

        # Step 1: seed the on-disk image. Write data, make it stable, and checkpoint so the stable
        # btrees are populated and clean.
        self.write_all("aaaaa" * 100, 100)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(100)}')
        self.session.checkpoint()

        # Step 2: dirty the pages again with content that is still visible to the coming checkpoint,
        # and make it stable. This is the content the checkpoint's snapshot must see so the page is not
        # trivially skipped.
        self.write_all("bbbbb" * 100, 150)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(150)}')

        # Step 3: run a single checkpoint in the background. It publishes its eviction snapshot during prepare,
        # then stalls before walking the trees. With many tables the checkpoint walks them sequentially,
        # so eviction has a window to reconcile a not-yet-walked table's pages under the published snapshot
        # before the checkpoint reaches them.
        snapshots_before = self.read_stat(stat.conn.checkpoint_snapshot_acquired)
        ckpt_done = threading.Event()
        ckpt_thread = checkpoint_thread(self.conn, ckpt_done, checkpoint_count_max=1)
        try:
            ckpt_thread.start()

            # Wait until the checkpoint has acquired its snapshot before writing content it cannot see.
            while self.read_stat(stat.conn.checkpoint_snapshot_acquired) == snapshots_before:
                time.sleep(0.1)

            # Step 4: write above the stable timestamp while the checkpoint stalls. These updates
            # commit after the checkpoint's snapshot is published, so they are invisible to it. Eviction
            # writes the visible content to a durable image but keeps these in memory, leaving the page
            # dirty, durable, and stamped with the checkpoint's snapshot generation.
            self.write_all("ccccc" * 100, 200)

            self.evict_all()
        finally:
            ckpt_done.set()
            ckpt_thread.join()

        # The checkpoint should have skipped re-reconciling at least one page eviction already
        # reconciled under its snapshot.
        self.assertGreater(
            self.read_stat(stat.conn.checkpoint_pages_reconciliation_skipped_evict_snapshot), 0)

if __name__ == '__main__':
    wttest.run()
