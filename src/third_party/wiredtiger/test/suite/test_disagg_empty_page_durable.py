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

import threading, time, wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class

# test_disagg_empty_page_durable.py
#   A delete that empties a leaf must reach the stable checkpoint even when the
#   eviction that first reconciled the empty page failed before completing.
#
#   A leader deletes the only row on a leaf while a checkpoint is running. An
#   eviction reconciles the now empty leaf and is forced to fail before wrapup.
#   The leader then starts a planned step-down: a newer write is mirrored to the
#   stable table, and the leaf is evicted again under the step-down checkpoint's
#   snapshot. After stepping down and back up, the deleted row must stay deleted.
@disagg_test_class
class test_disagg_empty_page_durable(wttest.WiredTigerTestCase):
    uri_base = 'test_disagg_empty_page_durable'
    uri = 'layered:' + uri_base
    stable_uri = 'file:' + uri_base + '.wt_stable'
    conn_base_config = ('cache_size=1GB,statistics=(all),precise_checkpoint=true,'
                        'preserve_prepared=true,'
                        'page_delta=(leaf_page_delta=true,internal_page_delta=true),')
    conn_config = conn_base_config + \
        'disaggregated=(role="leader",stepdown_write_mirroring=true)'
    create_config = ('key_format=S,value_format=S,allocation_size=512,leaf_page_max=512,'
                     'leaf_value_max=5120,internal_page_max=4096,memory_page_max=5MB,'
                     'split_pct=67')

    # The victim's large value keeps it alone on its leaf; the neighbors sort on either side.
    left_key = '0000552898.00/opqrstuvwxyza'
    victim_key = '0000552900.00/opqrstuvwxyza'
    insert_key = '0000552900.03/opqrstuvwxyza'
    right_key = '0000552901.00/opqrstuvwxyza'
    bulk_value = 'B' * 1442
    small_value = 's' * 177

    def set_ts(self, oldest, stable):
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(oldest)},'
                                f'stable_timestamp={self.timestamp_str(stable)}')

    def stat(self, key):
        with wttest.open_cursor(self.session, 'statistics:') as c:
            return c[key][2]

    def write(self, kvs, commit_ts, read_ts=None):
        c = self.session.open_cursor(self.uri)
        cfg = f'read_timestamp={self.timestamp_str(read_ts)}' if read_ts else None
        self.session.begin_transaction(cfg)
        for k, v in kvs.items():
            if v is None:
                c.set_key(k)
                self.assertEqual(c.remove(), 0)
            else:
                c[k] = v
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(commit_ts)}')
        c.close()

    # Evict the stable leaf holding the key from the application thread.
    def evict(self, key):
        c = self.session.open_cursor(self.stable_uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        c.set_key(key)
        c.search()
        c.reset()
        c.close()
        self.session.rollback_transaction()

    # Start a checkpoint and return once it has published its snapshot. Checkpoint_slow then holds
    # it for ten seconds before it visits any tree.
    def start_slow_checkpoint(self):
        gen = self.stat(stat.conn.checkpoint_generation)
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_slow]')
        session = self.conn.open_session()
        thread = threading.Thread(target=session.checkpoint)
        thread.start()
        deadline = time.time() + 60
        while (self.stat(stat.conn.checkpoint_generation) == gen or
               self.stat(stat.conn.checkpoint_snapshot_acquired) == 0 or
               self.stat(stat.conn.checkpoint_prep_running) != 0):
            self.assertLess(time.time(), deadline, 'checkpoint did not publish its snapshot')
            time.sleep(0.01)
        return thread, session

    def finish_slow_checkpoint(self, thread, session):
        thread.join()
        session.close()
        self.conn.reconfigure('timing_stress_for_test=[]')

    def pickup(self):
        self.disagg_advance_checkpoint_and_wait(self.conn)

    def victim_visible(self):
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c.set_key(self.victim_key)
        ret = c.search()
        self.session.rollback_transaction()
        c.close()
        return ret == 0

    def test_empty_page_durable(self):
        self.ignoreStdoutPattern('Picking up the same checkpoint again')
        self.set_ts(1, 1)
        self.session.create(self.uri, self.create_config)
        self.write({self.victim_key: self.bulk_value, self.left_key: self.small_value,
                    self.right_key: self.small_value}, 10)
        self.set_ts(20, 20)
        self.session.checkpoint()

        # Step down and back up, and read the leaf back from the checkpoint.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.pickup()
        self.set_ts(20, 20)
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.checkpoint()
        self.evict(self.victim_key)
        self.assertTrue(self.victim_visible())

        # Delete the victim while a checkpoint is running.
        self.write({self.right_key: self.bulk_value}, 30)
        self.set_ts(31, 31)
        thread, session = self.start_slow_checkpoint()
        self.write({self.victim_key: None}, 35, read_ts=33)
        self.finish_slow_checkpoint(thread, session)

        # Reconcile the empty leaf and fail the eviction before wrapup.
        self.set_ts(40, 40)
        fails = self.stat(stat.conn.eviction_fail_in_reconciliation)
        try:
            self.conn.reconfigure('timing_stress_for_test=[failpoint_rec_before_wrapup],'
                                  'debug_mode=(timing_stress_force=true)')
            self.evict(self.victim_key)
        finally:
            self.conn.reconfigure(
                'timing_stress_for_test=[],debug_mode=(timing_stress_force=false)')
        self.assertGreater(self.stat(stat.conn.eviction_fail_in_reconciliation), fails,
            'the eviction of the empty leaf did not fail')

        # Start the step-down. The newer write lands in ingest and is mirrored to stable.
        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(42))
        self.write({self.insert_key: self.small_value}, 50)
        self.set_ts(40, 42)

        # Evict the leaf under the step-down checkpoint's snapshot.
        app_ckpt = self.stat(stat.conn.application_evict_checkpoint_snapshot)
        thread, session = self.start_slow_checkpoint()
        self.evict(self.victim_key)
        self.finish_slow_checkpoint(thread, session)
        self.assertGreater(self.stat(stat.conn.application_evict_checkpoint_snapshot), app_ckpt,
            'the leaf was not evicted under the checkpoint snapshot')

        # Step down, pick up the step-down checkpoint, and step back up.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.pickup()
        self.assertFalse(self.victim_visible(), 'deleted row is visible on the follower')
        self.set_ts(60, 60)
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.checkpoint()

        self.assertFalse(self.victim_visible(), 'deleted row is visible after step-up')
