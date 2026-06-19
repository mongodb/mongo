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
#
# Rolling back a follower-claimed prepared transaction is durable across
# checkpoints. After the rollback completes and a new checkpoint is taken,
# reopening prepared_discover against that checkpoint must not surface the
# already-rolled-back prepared id.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover15(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def _open_follower(self, checkpoint_meta):
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn

    def _checkpoint(self, conn):
        s = conn.open_session()
        s.checkpoint()
        s.close()

    def _discover_prepared_ids(self, conn):
        """Return every prepared id that prepared_discover surfaces."""
        s = conn.open_session()
        ids = []
        try:
            c = s.open_cursor('prepared_discover:')
        except wiredtiger.WiredTigerError:
            # No prepared content present; discover is effectively empty.
            s.close()
            return ids
        while c.next() == 0:
            ids.append(c.get_key())
        c.close()
        s.close()
        return ids

    def test_rolled_back_claim_does_not_resurface(self):
        prepared_id = 17304
        # Multiple keys exercise the rollback path on more than one record
        # in a single test run.
        prepared_keys = [50, 500, 5000, 50000]

        # Leader: write a baseline, then prepare an update over the same
        # keys, checkpoint while the transaction is prepared, and roll the
        # leader's prepare back. The checkpoint captures the prepared cells
        # on the stable disk image so the follower can discover them.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for k in prepared_keys:
            cursor[k] = 'baseline_value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        for k in prepared_keys:
            cursor[k] = 'prepared_value'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self._checkpoint(self.conn)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        self.session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        # Skip the final checkpoint so the follower observes the prepared
        # cells exactly as the captured checkpoint persisted them.
        self.conn.close('debug=(skip_checkpoint=true)')

        # Follower: discover the prepared transaction, claim it, and roll
        # the claim back. The leader's prepared id should be surfaced
        # exactly once.
        conn_follow = self._open_follower(checkpoint_meta)

        discover_session = conn_follow.open_session()
        discover_cursor = discover_session.open_cursor('prepared_discover:')
        claim_session = conn_follow.open_session()

        discovered = []
        while discover_cursor.next() == 0:
            pid = discover_cursor.get_key()
            discovered.append(pid)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))
        discover_cursor.close()
        discover_session.close()
        self.assertEqual(discovered, [prepared_id],
            "Leader's prepared cell must be discoverable on the follower.")

        claim_session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(210))
        claim_session.close()

        # Step up and take a new checkpoint. Advance stable past the
        # rollback timestamp so the rolled-back state is durable in the
        # newly written checkpoint.
        conn_follow.reconfigure('disaggregated=(role="leader")')
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

        # Step back down and re-run prepared_discover against the new
        # checkpoint. Since the prepared transaction was rolled back, it
        # must not be surfaced again.
        conn_follow.reconfigure('disaggregated=(role="follower")')
        resurrected = self._discover_prepared_ids(conn_follow)
        conn_follow.close()

        self.assertNotIn(
            prepared_id, resurrected,
            "A rolled-back prepared transaction must not be surfaced by a "
            "later prepared_discover on the same checkpoint chain "
            f"(prepared_discover returned {resurrected}).")
