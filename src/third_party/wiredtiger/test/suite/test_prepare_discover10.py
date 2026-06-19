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
# A prepared transaction reclaimed on a follower via claim_prepared_id must survive
# step-up. The reclaim session has no transaction id, so the ingest drain at step-up
# must match it by prepared id to patch its operations onto the stable btree.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover10(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename

    resolve_scenarios = [
        ('commit',   dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]
    multi_table_scenarios = [
        ('single_table', dict(multi_table=False)),
        ('multi_table',  dict(multi_table=True)),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolve_scenarios, multi_table_scenarios)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    @property
    def uri_b(self):
        return 'layered:' + self.tablename + '_b'

    @property
    def _uris(self):
        """URI(s) exercised by this run -- one table or two, based on the multi_table scenario."""
        return [self.uri] + ([self.uri_b] if self.multi_table else [])

    def _open_follower(self, checkpoint_meta):
        """Open a follower connection and apply the given checkpoint metadata."""
        conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn_follow

    def _checkpoint(self, conn):
        """Take a checkpoint on the given connection."""
        session = conn.open_session()
        session.checkpoint()
        session.close()

    def test_claimed_prepare_insert_survives_step_up(self):
        """
        Prepared INSERT (keys 4-6) is captured in the leader's checkpoint, reclaimed on
        the follower via claim_prepared_id, then resolved after step-up. Verifies the
        resolve sequence completes without error.
        """
        prepared_id = 12345
        uris = self._uris

        # ---- Phase 1 (Leader) ----
        # Commit baseline keys 1-3, prepare INSERT on keys 4-6, checkpoint with the
        # prepare captured, then roll back so the follower can reclaim it by prepared id.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp='  + self.timestamp_str(50))

        for uri in uris:
            self.session.create(uri, 'key_format=i,value_format=S')
        leader_cursors = [self.session.open_cursor(uri) for uri in uris]

        self.session.begin_transaction()
        for c in leader_cursors:
            c[1] = 'committed_value_1'
            c[2] = 'committed_value_2'
            c[3] = 'committed_value_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        for c in leader_cursors:
            c[4] = 'prepared_value_4'
            c[5] = 'prepared_value_5'
            c[6] = 'prepared_value_6'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(prepared_id))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self._checkpoint(self.conn)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))

        for c in leader_cursors:
            c.close()
        # Close without a final checkpoint so the follower loads exactly the state above.
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower: discover and reclaim) ----
        # Walk prepared_discover and reclaim each surfaced id on a dedicated session.
        # Reclaiming sets prepared id but leaves transaction id unset.
        conn_follow = self._open_follower(checkpoint_meta)
        discover_session = conn_follow.open_session()
        discover_cursor = discover_session.open_cursor('prepared_discover:')
        claim_session = conn_follow.open_session()

        discovered = []
        while discover_cursor.next() == 0:
            pid = discover_cursor.get_key()
            discovered.append(pid)
            # Every surfaced prepared id must be claimed before the cursor closes.
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))

        self.assertEqual(discovered, [prepared_id])
        discover_cursor.close()
        discover_session.close()

        # ---- Phase 3 (Step up while the reclaim is live) ----
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # ---- Phase 4 (Resolve the reclaimed transaction) ----
        if self.commit:
            claim_session.timestamp_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(210))
            claim_session.commit_transaction()
        else:
            claim_session.rollback_transaction(
                'rollback_timestamp=' + self.timestamp_str(210))
        claim_session.close()

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

        # ---- Phase 5 (Validate data) ----
        # At ts 60, only the baseline keys 1-3 are visible; the prepared insert is
        # younger than the read. At ts 220 the resolution is observable: keys 4-6
        # carry their prepared values when committed and are absent when rolled back.
        read_session = conn_follow.open_session()
        read_cursors = [read_session.open_cursor(uri) for uri in uris]

        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for c in read_cursors:
            self.assertEqual(c[1], 'committed_value_1')
            self.assertEqual(c[2], 'committed_value_2')
            self.assertEqual(c[3], 'committed_value_3')
            for i in range(4, 7):
                c.set_key(i)
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        read_session.rollback_transaction()

        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(220))
        for c in read_cursors:
            self.assertEqual(c[1], 'committed_value_1')
            self.assertEqual(c[2], 'committed_value_2')
            self.assertEqual(c[3], 'committed_value_3')
            for i in range(4, 7):
                c.set_key(i)
                if self.commit:
                    self.assertEqual(c.search(), 0)
                    self.assertEqual(c.get_value(), f'prepared_value_{i}')
                else:
                    self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        read_session.rollback_transaction()

        for c in read_cursors:
            c.close()
        read_session.close()

        conn_follow.close()
