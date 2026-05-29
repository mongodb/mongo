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

# test_layered_stepup06.py
#   When two prepared sessions share the same prepared_id, a follower
#   stepping up to leader must resolve all of them.
#
#   The follower holds two prepared sessions with prepared_id=42:
#   top_session (no writes) and work_session (table writes). Without the
#   fix, step-up fails to resolve work_session's writes, causing step-up
#   to fail with a conflict.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_layered_stepup06(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup06'

    resolve_scenarios = [
        ('commit',   dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]
    disagg_storages = gen_disagg_storages('test_layered_stepup06', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolve_scenarios)

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

    def test_split_prepared_survives_step_up(self):
        """
        When a write-free prepared session (top_session) and a writing prepared
        session (work_session) share the same prepared_id, step-up must succeed
        and both sessions must be resolvable afterward.
        """
        # ---- Phase 1 (Leader) ----------------------------------------
        # Commit base values and checkpoint with no prepared transactions.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for i in range(1, 4):
            c[i] = f'base_{i}'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))
        c.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))
        self._checkpoint(self.conn)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower: set up split-prepared state) ----------
        conn_f = self._open_follower(checkpoint_meta)

        # top_session must be opened before work_session to trigger the
        # failure scenario during step-up.
        top_session = conn_f.open_session()
        work_session = conn_f.open_session()

        # top_session makes no writes: it is a prepared session that carries
        # only the prepared_id with no data modifications.
        top_session.begin_transaction()
        top_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(42))

        # work_session writes the actual data with the same prepared_id=42.
        c_work = work_session.open_cursor(self.uri)
        work_session.begin_transaction()
        for i in range(4, 7):
            c_work[i] = f'split_{i}'
        work_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(42))

        # ---- Phase 3 (Step up) ----------------------------------------
        # Without the fix, step-up fails to resolve work_session's writes,
        # causing it to fail with a conflict.
        conn_f.reconfigure('disaggregated=(role="leader")')

        # ---- Phase 4 (Resolve both prepared transactions) -------------
        if self.commit:
            top_session.timestamp_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(210))
            top_session.commit_transaction()

            work_session.timestamp_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(210))
            work_session.commit_transaction()
        else:
            top_session.rollback_transaction(
                'rollback_timestamp=' + self.timestamp_str(210))
            work_session.rollback_transaction(
                'rollback_timestamp=' + self.timestamp_str(210))

        c_work.close()
        top_session.close()
        work_session.close()

        conn_f.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_f)

        # ---- Phase 5 (Verify) ----------------------------------------
        read_s = conn_f.open_session()
        rc = read_s.open_cursor(self.uri)

        # Base values (committed at ts=60) are always visible.
        read_s.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for i in range(1, 4):
            self.assertEqual(rc[i], f'base_{i}')
        for i in range(4, 7):
            rc.set_key(i)
            self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND)
        read_s.rollback_transaction()

        # Keys 4-6 are visible on commit, absent on rollback.
        read_s.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        for i in range(4, 7):
            rc.set_key(i)
            if self.commit:
                self.assertEqual(rc.search(), 0)
                self.assertEqual(rc.get_value(), f'split_{i}')
            else:
                self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND)
        read_s.rollback_transaction()

        rc.close()
        read_s.close()
        conn_f.close()
