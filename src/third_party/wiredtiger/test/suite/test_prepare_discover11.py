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
# A prepared transaction claimed and committed by a follower must be
# readable at its commit timestamp after the follower steps up to leader,
# even when the stable timestamp has advanced past the durable timestamp
# and the affected pages have been evicted from memory.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover11(wttest.WiredTigerTestCase):
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
        conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn_follow

    def _checkpoint(self, conn):
        session = conn.open_session()
        session.checkpoint()
        session.close()

    def test_committed_prepared_visible_after_stepup(self):
        """
        A prepared transaction captured in the leader's checkpoint is claimed and
        committed by the follower. After the stable timestamp advances past the
        durable timestamp and the affected pages are evicted from memory, the
        follower steps up. The committed keys must be readable at their commit
        timestamp and invisible before their prepare timestamp.
        """
        # ---- Phase 1 (Leader) ----
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp='  + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = 'committed_1'
        cursor[2] = 'committed_2'
        cursor[3] = 'committed_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        cursor[4] = 'prepared_4'
        cursor[5] = 'prepared_5'
        cursor[6] = 'prepared_6'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(99999))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self._checkpoint(self.conn)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Roll back so the follower owns the claim.
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower: claim and commit BEFORE step-up) ----
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
            claim_session.timestamp_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(200))
            claim_session.commit_transaction()

        self.assertEqual(discovered, [99999])
        discover_cursor.close()
        discover_session.close()
        claim_session.close()

        # ---- Phase 3 (Advance stable_ts past durable_ts, evict page) ----
        # Advance stable_ts past the durable timestamp of the committed prepared
        # transaction (250 > 200) so that the committed updates become eligible
        # for eviction. Then force the affected pages out of memory so that
        # step-up drain must resolve the prepared transaction from disk.
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))

        # Force eviction of the affected page by reading through a session with
        # release_evict_page=true.
        evict_session = conn_follow.open_session('debug=(release_evict_page=true)')
        evict_cursor = evict_session.open_cursor(self.uri)
        for i in range(4, 7):
            evict_cursor.set_key(i)
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()
        evict_session.close()

        # ---- Phase 4 (Step up) ----
        conn_follow.reconfigure('disaggregated=(role="leader")')

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(300))
        self._checkpoint(conn_follow)

        # ---- Phase 5 (Validate data) ----
        # Keys 1-3 committed at ts=60: visible at ts>=60, not before.
        # Keys 4-6 committed at ts=200: invisible before prepare_ts=100, visible at ts>=200.
        read_session = conn_follow.open_session()
        c = read_session.open_cursor(self.uri)

        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for i in range(1, 4):
            self.assertEqual(c[i], f'committed_{i}')
        for i in range(4, 7):
            c.set_key(i)
            self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        read_session.rollback_transaction()

        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(220))
        for i in range(1, 4):
            self.assertEqual(c[i], f'committed_{i}')
        for i in range(4, 7):
            self.assertEqual(c[i], f'prepared_{i}')
        read_session.rollback_transaction()

        c.close()
        read_session.close()
        conn_follow.close()
