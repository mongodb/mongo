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
from helper_disagg import DisaggConfigMixin, disagg_test_class

# test_disagg_checkpoint_size19.py
#   Exercises the disagg-shared checkpoint-URI open path that guards against
#   clobbering a live running size total on checkpoint pick-up.
#
#   Internal disagg paths (notably checkpoint pick-up and role-switch step-up)
#   open stable files via a suffix URI of the form
#   file:X.wt_stable/WiredTigerCheckpoint.N. The btree open path strips the
#   checkpoint suffix and recognizes the open as a checkpoint-cursor open, but
#   the block manager may reuse a cached block handle already held by a live
#   writer for the same base file. Without the guard, the open would
#   unconditionally reset the shared running size total to the checkpoint's
#   recorded size, clobbering bytes accumulated since the last checkpoint by
#   the live writer -- a subsequent eviction would then underflow the running
#   total.
#
#   The full underflow reproducer requires a single-process scenario where a
#   live writer and a checkpoint pick-up share a block handle (the format-stress
#   disagg-switch workload). This Python test exercises the role-swap and
#   pick-up code paths across multiple rounds with two connections in the same
#   process and live writes between transitions, guarding the open-path code
#   from regressing. The deterministic underflow catcher for this bug lives in
#   test/format under disagg.mode=switch.

@disagg_test_class
class test_disagg_checkpoint_size19(DisaggConfigMixin, wttest.WiredTigerTestCase):

    nrows = 500

    conn_base_config = (
        'statistics=(all),'
        'disaggregated=(lose_all_my_data=true),'
        'page_delta=(delta_pct=90,leaf_page_delta=true,max_consecutive_delta=32),'
        'cache_size=200MB,'
    )
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_follow_config = conn_base_config + 'disaggregated=(role="follower")'

    table_config = ('key_format=S,value_format=S,'
                    'leaf_page_max=4KB,internal_page_max=8KB,'
                    'memory_page_max=200MB,split_pct=50')

    uri_base = 'test_disagg_ckpt_size19'
    uri = 'layered:' + uri_base

    def insert_rows(self, session, start, count, value_char):
        c = session.open_cursor(self.uri)
        value = value_char * 1024
        for i in range(start, start + count):
            c[f'key{i:08d}'] = value
        c.close()

    def evict_page(self, session, key):
        evict = session.open_cursor(self.uri, None, 'debug=(release_evict)')
        session.begin_transaction()
        evict.set_key(key)
        evict.search()
        evict.reset()
        evict.close()
        session.rollback_transaction()

    def test_role_swap_pickup_does_not_clobber_live_size(self):
        # Picking up a checkpoint we already have is benign for this test:
        # the open-path code we want to exercise still runs.
        self.ignoreStdoutPattern('Picking up the same checkpoint again')

        # Two connections in the same process.
        self.session.create(self.uri, self.table_config)
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() +
                                           ',create,' + self.conn_follow_config)
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)

        try:
            # Seed: leader writes initial data and checkpoints (small ckpt.size).
            self.insert_rows(self.session, 0, 50, 'A')
            self.session.checkpoint()
            self.disagg_advance_checkpoint(conn_follow)

            # Track which connection is currently the leader.
            leader_conn = self.conn
            leader_sess = self.session
            follower_conn = conn_follow
            follower_sess = session_follow

            for round_idx in range(3):
                # Grow the live size on the current leader: many updates and
                # appends without an intervening checkpoint accumulate bytes in
                # the block handle's running total.
                char = chr(ord('B') + round_idx * 2)
                self.insert_rows(leader_sess, 0, self.nrows, char)
                self.insert_rows(leader_sess, self.nrows * (round_idx + 1),
                                 self.nrows, chr(ord(char) + 1))

                # Swap roles. The new leader's step-up opens stable files via
                # the disagg-shared suffix URI -- the path the fix protects.
                # The outgoing leader retains cached block handles for the base
                # URI; if it later picks up newer checkpoints, those opens must
                # not reset the running total that live writes have accumulated.
                self.disagg_switch_follower_and_leader(follower_conn, leader_conn)
                leader_conn, follower_conn = follower_conn, leader_conn
                leader_sess, follower_sess = follower_sess, leader_sess

                # Force evictions of leaf pages on the new leader.
                # Reconciliation decrements the running total for each
                # evicted chain; an underflow here would abort.
                for k in (0, self.nrows // 4, self.nrows // 2,
                          3 * self.nrows // 4, self.nrows - 1):
                    try:
                        self.evict_page(leader_sess, f'key{k:08d}')
                    except Exception:
                        pass

                # Checkpoint on the new leader to flush the page-discard
                # paths in the disagg block manager.
                leader_sess.checkpoint()

                # Have the outgoing leader (current follower) pick up the new
                # checkpoint. This re-exercises the suffix-URI open on a
                # connection whose cached block handles still belong to its
                # previous leader era -- exactly the handle-sharing scenario
                # the guard protects.
                self.disagg_advance_checkpoint(follower_conn, leader_conn)

            # Sanity: data is still readable on whichever conn is leader.
            c = leader_sess.open_cursor(self.uri)
            count = sum(1 for _ in c)
            c.close()
            self.assertGreater(count, 0,
                'No rows readable after role-swap rounds')
        finally:
            session_follow.close()
            conn_follow.close()
