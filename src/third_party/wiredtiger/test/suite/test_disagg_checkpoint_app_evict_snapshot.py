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


import threading, time
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wtthread import checkpoint_thread
from wiredtiger import stat

# test_disagg_checkpoint_app_evict_snapshot.py
#   Under precise checkpoint, an application thread that reconciles a page during eviction should use
#   the checkpoint's published snapshot as its visibility horizon, matching the eviction workers. This
#   is only done for disaggregated storage, where reconciliation always leaves a durable image behind.
#
#   The checkpoint publishes its eviction snapshot during prepare, then stalls before walking the tree.
#   We force eviction inside that window so it reconciles under the checkpoint's snapshot.
@disagg_test_class
class test_disagg_checkpoint_app_evict_snapshot(wttest.WiredTigerTestCase):
    uri = 'layered:checkpoint_app_evict_snapshot'
    nrows = 2000

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # A large cache keeps the eviction workers idle so the forced application-thread eviction below is
    # the eviction that reconciles the pages.
    def conn_config(self):
        return 'cache_size=1GB,statistics=(all),precise_checkpoint=true,' \
            'timing_stress_for_test=[checkpoint_slow],disaggregated=(role="leader"),'

    def read_stat(self, stat_key):
        with wttest.open_cursor(self.session, "statistics:") as stat_cursor:
            return stat_cursor[stat_key][2]

    def key(self, i):
        return f'key{i:08d}'

    def evict_all(self):
        # Force eviction of every leaf page from an application thread.
        s = self.conn.open_session("debug=(release_evict_page=true)")
        s.begin_transaction()
        evict_cursor = s.open_cursor(self.uri, None, None)
        for i in range(1, self.nrows + 1):
            evict_cursor.set_key(self.key(i))
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()
        s.rollback_transaction()
        s.close()

    def test_app_evict_checkpoint_snapshot(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        value_a = "aaaaa" * 100
        stable = 100

        # Write data visible to the checkpoint snapshot and make it stable.
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cursor[self.key(i)] = value_a
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(stable)}')
        cursor.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(stable)}')

        # Run a single checkpoint in the background. It publishes its eviction snapshot during prepare,
        # then stalls before walking the tree.
        snapshots_before = self.read_stat(stat.conn.checkpoint_snapshot_acquired)
        ckpt_done = threading.Event()
        ckpt_thread = checkpoint_thread(self.conn, ckpt_done, checkpoint_count_max=1)
        try:
            ckpt_thread.start()

            # Wait until the checkpoint has acquired its snapshot and entered the stall, then force
            # eviction so it reconciles under the checkpoint's snapshot.
            while self.read_stat(stat.conn.checkpoint_snapshot_acquired) == snapshots_before:
                time.sleep(0.1)
            self.evict_all()
        finally:
            ckpt_done.set()
            ckpt_thread.join()

        # The application-thread eviction should have used the checkpoint's published snapshot.
        self.assertGreater(self.read_stat(stat.conn.application_evict_checkpoint_snapshot), 0)

if __name__ == '__main__':
    wttest.run()
