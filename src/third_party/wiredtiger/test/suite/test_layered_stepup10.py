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

# test_layered_stepup10.py
#   Regression test for disaggregated btree handling across step-down/step-up.
#   On step-down every disaggregated btree is marked readonly so the follower
#   does not checkpoint it, and the handle is marked outdated so the next leader
#   opens a fresh one. A handle left readonly across step-up is otherwise
#   skipped by the next step-up checkpoint, leaving its disaggregated block-size
#   accounting stale and tripping the checkpoint-size assertion
#   (ckpt->size == __wt_block_disagg_get_size) on the second cycle. A single
#   fresh-follower step-up does not exercise this -- two full
#   leader->follower->leader cycles on the same connection are required.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, Oplog
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_stepup10(wttest.WiredTigerTestCase):
    conn_base_config = (
        ',create,statistics=(all),'
        'precise_checkpoint=true,'
        'preserve_prepared=true,'
    )

    disagg_storages = gen_disagg_storages('test_layered_stepup10', disagg_only=True)

    sizes = [
        ('small', dict(multiplier=1)),
        ('large', dict(multiplier=10)),
    ]

    scenarios = make_scenarios(disagg_storages, sizes)

    @property
    def base_config(self):
        return self.extensionsConfig() + self.conn_base_config

    def conn_config(self):
        return self.base_config + 'disaggregated=(role="leader")'

    @property
    def conn_follower_config(self):
        return self.base_config + 'disaggregated=(role="follower")'

    def test_drain_multiple_step_up_cycles(self):
        """
        Two full follower->leader transitions on the same table verify that
        the ingest is properly cleared after the first drain and that the
        second drain does not re-drain or lose data from the first cycle.

        Uses the single-connection follower->leader pattern; no
        disagg_advance_checkpoint is needed.
        """
        uri = 'layered:test_layered_stepup10_multi_cycle'
        n_batch = 50 * self.multiplier

        oplog = Oplog()
        t = oplog.add_uri(uri)

        # --- Stable baseline (batch 1) ---
        oplog.insert(t, n_batch)
        self.session.create(uri, 'key_format=S,value_format=S')
        oplog.apply(self, self.session, 0, n_batch)
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        self.session.checkpoint()

        # --- Cycle 1: step down, write batch 2, step up ---
        self.conn.reconfigure('disaggregated=(role="follower")')
        oplog.insert(t, n_batch)
        session_c1 = self.conn.open_session('')
        oplog.apply(self, session_c1, n_batch, n_batch)

        self.conn.reconfigure('disaggregated=(role="leader")')
        # Checkpoint advances last_checkpoint_timestamp so cycle 2's batch can be
        # drained (its timestamps are strictly above this stable_timestamp).
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        session_c1.checkpoint()
        session_c1.close()

        # --- Cycle 2: step down, write batch 3, step up ---
        self.conn.reconfigure('disaggregated=(role="follower")')
        oplog.insert(t, n_batch)
        session_c2 = self.conn.open_session('')
        oplog.apply(self, session_c2, 2 * n_batch, n_batch)

        self.conn.reconfigure('disaggregated=(role="leader")')
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        session_c2.checkpoint()
        session_c2.close()

        # Verify all three batches visible on the current leader connection.
        verify_session = self.conn.open_session('')
        oplog.check(self, verify_session, 0, 3 * n_batch)
        verify_session.close()
