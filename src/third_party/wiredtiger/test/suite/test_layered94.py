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
# test_layered94.py
#   Verifies that a prepared transaction active on a follower at step-up time can be
#   committed or rolled back after step-up completes.
#
#   Covered operations (each run as a cross-product of all scenario dimensions):
#     - prepared INSERT on new keys
#     - prepared UPDATE on existing keys
#     - prepared DELETE on existing keys
#
#   Scenario dimensions:
#     resolve:       commit | rollback
#     in_checkpoint: True   prepare captured by the last checkpoint
#                    False  prepare made after the last checkpoint
#     multi_table:   True   the prepared transaction also covers a second layered table

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered94(wttest.WiredTigerTestCase):
    tablename = 'test_layered94'
    uri = 'layered:' + tablename

    resolve_scenarios = [
        ('commit',   dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]
    checkpoint_scenarios = [
        ('in_checkpoint',     dict(in_checkpoint=True)),
        ('not_in_checkpoint', dict(in_checkpoint=False)),
    ]
    multi_table_scenarios = [
        ('single_table', dict(multi_table=False)),
        ('multi_table',  dict(multi_table=True)),
    ]
    disagg_storages = gen_disagg_storages('test_layered94', disagg_only=True)
    scenarios = make_scenarios(
        disagg_storages, resolve_scenarios, checkpoint_scenarios, multi_table_scenarios)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    @property
    def uri_b(self):
        return 'layered:' + self.tablename + '_b'

    @property
    def _uris(self):
        """URI(s) exercised by this run  one table or two, based on the multi_table scenario."""
        return [self.uri] + ([self.uri_b] if self.multi_table else [])

    # -----------------------------------------------------------------
    # Helpers
    # -----------------------------------------------------------------

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

    def _resolve_prepared(self, session_f, cursors):
        """Commit or roll back the prepared transaction, then close all cursors and the session."""
        if self.commit:
            session_f.timestamp_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(210))
            session_f.commit_transaction()
        else:
            session_f.rollback_transaction(
                'rollback_timestamp=' + self.timestamp_str(210))
        for cursor in cursors:
            cursor.close()
        session_f.close()

    # -----------------------------------------------------------------
    # Tests
    # -----------------------------------------------------------------

    def test_prepare_insert_survives_step_up(self):
        """
        A replicated prepared INSERT (keys 4-6) is still active when the follower steps up.

        When in_checkpoint=True the prepare was captured in the last checkpoint.  When
        in_checkpoint=False the prepare was made after the last checkpoint, so it is absent
        from the checkpoint the follower loads.

        After step-up the transaction must be committable (making the inserted keys visible)
        or rollback-able (leaving them absent).  When multi_table=True the same prepared
        transaction also covers a second table; both tables must reflect the correct state.
        """
        uris = self._uris

        # ---- Phase 1 (Leader) ----
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp='  + self.timestamp_str(50))

        for uri in uris:
            self.session.create(uri, 'key_format=i,value_format=S')
        leader_cursors = [self.session.open_cursor(uri) for uri in uris]

        # Commit keys 1-3 as base data on all tables.
        self.session.begin_transaction()
        for c in leader_cursors:
            c[1] = 'committed_value_1'
            c[2] = 'committed_value_2'
            c[3] = 'committed_value_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        if self.in_checkpoint:
            # Prepare INSERT on keys 4-6, advance stable past the prepare timestamp, and
            # checkpoint so the prepare is included in the checkpoint.
            self.session.begin_transaction()
            for c in leader_cursors:
                c[4] = 'prepared_value_4'
                c[5] = 'prepared_value_5'
                c[6] = 'prepared_value_6'
            self.session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(100) +
                ',prepared_id='      + self.prepared_id_str(123))
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
            # Roll back the leader's prepare so it does not conflict on connection close.
            self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        else:
            # Checkpoint before any prepare  keys 4-6 are absent from the checkpoint.
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        for c in leader_cursors:
            c.close()
        # Close without a final checkpoint so the follower loads exactly the state above.
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower) ----
        # Open a follower loaded from the checkpoint and begin a prepared transaction using the
        # same prepare_timestamp and prepared_id as the leader.  Leave it active so there is a
        # live prepared INSERT present when step-up fires in Phase 3.
        conn_follow = self._open_follower(checkpoint_meta)
        session_f = conn_follow.open_session()
        follower_cursors = [session_f.open_cursor(uri) for uri in uris]
        session_f.begin_transaction()
        for c in follower_cursors:
            c[4] = 'prepared_value_4'
            c[5] = 'prepared_value_5'
            c[6] = 'prepared_value_6'
        session_f.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(123))

        # ---- Phase 3 (Step-up while prepare is live) ----
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # ---- Phase 4 (Resolve) ----
        self._resolve_prepared(session_f, follower_cursors)

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

        # ---- Verify ----
        read_session = conn_follow.open_session()
        read_cursors = [read_session.open_cursor(uri) for uri in uris]

        # Keys 1-3 were committed at ts=60 and are always visible on all tables.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for rc in read_cursors:
            self.assertEqual(rc[1], 'committed_value_1')
            self.assertEqual(rc[2], 'committed_value_2')
            self.assertEqual(rc[3], 'committed_value_3')
            for i in range(4, 7):
                rc.set_key(i)
                self.assertEqual(wiredtiger.WT_NOTFOUND, rc.search())
        read_session.rollback_transaction()

        # After resolve: commit makes keys 4-6 visible on all tables; rollback keeps them absent.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        for rc in read_cursors:
            for i in range(4, 7):
                rc.set_key(i)
                if self.commit:
                    self.assertEqual(0, rc.search())
                    self.assertEqual(f'prepared_value_{i}', rc.get_value())
                else:
                    self.assertEqual(wiredtiger.WT_NOTFOUND, rc.search())
        read_session.rollback_transaction()

        for rc in read_cursors:
            rc.close()
        read_session.close()
        conn_follow.close()

    def test_prepare_update_survives_step_up(self):
        """
        A replicated prepared UPDATE (keys 1-3) is still active when the follower steps up.

        When in_checkpoint=True the prepare was captured in the last checkpoint.  When
        in_checkpoint=False the prepare was made after the last checkpoint, so it is absent
        from the checkpoint the follower loads.

        After step-up the transaction must be committable (exposing updated values) or
        rollback-able (restoring original values).  When multi_table=True the same prepared
        transaction also covers a second table; both tables must reflect the correct state.
        """
        uris = self._uris

        # ---- Phase 1 (Leader) ----
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp='  + self.timestamp_str(50))

        for uri in uris:
            self.session.create(uri, 'key_format=i,value_format=S')
        leader_cursors = [self.session.open_cursor(uri) for uri in uris]

        # Commit original values as base data on all tables.
        self.session.begin_transaction()
        for c in leader_cursors:
            c[1] = 'original_value_1'
            c[2] = 'original_value_2'
            c[3] = 'original_value_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        if self.in_checkpoint:
            # Prepare UPDATE on keys 1-3, advance stable past the prepare timestamp, and
            # checkpoint so the prepare is included in the checkpoint.
            self.session.begin_transaction()
            for c in leader_cursors:
                c[1] = 'updated_value_1'
                c[2] = 'updated_value_2'
                c[3] = 'updated_value_3'
            self.session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(100) +
                ',prepared_id='      + self.prepared_id_str(456))
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
            self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        else:
            # Checkpoint before any prepare  the prepare is absent from the checkpoint.
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        for c in leader_cursors:
            c.close()
        # Close without a final checkpoint so the follower loads exactly the state above.
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower) ----
        # Open a follower loaded from the checkpoint and begin a prepared transaction using the
        # same prepare_timestamp and prepared_id as the leader.  Leave it active so there is a
        # live prepared UPDATE present when step-up fires in Phase 3.
        conn_follow = self._open_follower(checkpoint_meta)
        session_f = conn_follow.open_session()
        follower_cursors = [session_f.open_cursor(uri) for uri in uris]
        session_f.begin_transaction()
        for c in follower_cursors:
            c[1] = 'updated_value_1'
            c[2] = 'updated_value_2'
            c[3] = 'updated_value_3'
        session_f.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(456))

        # ---- Phase 3 (Step-up while prepare is live) ----
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # ---- Phase 4 (Resolve) ----
        self._resolve_prepared(session_f, follower_cursors)

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

        # ---- Verify ----
        read_session = conn_follow.open_session()
        read_cursors = [read_session.open_cursor(uri) for uri in uris]

        # Original values are always visible at ts=60, regardless of resolution.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for rc in read_cursors:
            self.assertEqual(rc[1], 'original_value_1')
            self.assertEqual(rc[2], 'original_value_2')
            self.assertEqual(rc[3], 'original_value_3')
        read_session.rollback_transaction()

        # After resolve: commit exposes updated values; rollback restores original values.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        for rc in read_cursors:
            if self.commit:
                self.assertEqual(rc[1], 'updated_value_1')
                self.assertEqual(rc[2], 'updated_value_2')
                self.assertEqual(rc[3], 'updated_value_3')
            else:
                self.assertEqual(rc[1], 'original_value_1')
                self.assertEqual(rc[2], 'original_value_2')
                self.assertEqual(rc[3], 'original_value_3')
        read_session.rollback_transaction()

        for rc in read_cursors:
            rc.close()
        read_session.close()
        conn_follow.close()

    def test_prepare_delete_survives_step_up(self):
        """
        A replicated prepared DELETE (keys 1-3) is still active when the follower steps up.

        When in_checkpoint=True the prepare was captured in the last checkpoint.  When
        in_checkpoint=False the prepare was made after the last checkpoint, so it is absent
        from the checkpoint the follower loads.

        After step-up the transaction must be committable (making keys absent) or
        rollback-able (keeping original values).  When multi_table=True the same prepared
        transaction also covers a second table; both tables must reflect the correct state.
        """
        uris = self._uris

        # ---- Phase 1 (Leader) ----
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp='  + self.timestamp_str(50))

        for uri in uris:
            self.session.create(uri, 'key_format=i,value_format=S')
        leader_cursors = [self.session.open_cursor(uri) for uri in uris]

        # Commit original values as base data on all tables.
        self.session.begin_transaction()
        for c in leader_cursors:
            c[1] = 'original_value_1'
            c[2] = 'original_value_2'
            c[3] = 'original_value_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        if self.in_checkpoint:
            # Prepare DELETE on keys 1-3, advance stable past the prepare timestamp, and
            # checkpoint so the prepare is included in the checkpoint.
            self.session.begin_transaction()
            for c in leader_cursors:
                for i in range(1, 4):
                    c.set_key(i)
                    self.assertEqual(0, c.remove())
            self.session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(100) +
                ',prepared_id='      + self.prepared_id_str(789))
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
            self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        else:
            # Checkpoint before any prepare  the prepare is absent from the checkpoint.
            self._checkpoint(self.conn)
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        for c in leader_cursors:
            c.close()
        # Close without a final checkpoint so the follower loads exactly the state above.
        self.conn.close('debug=(skip_checkpoint=true)')

        # ---- Phase 2 (Follower) ----
        # Open a follower loaded from the checkpoint and begin a prepared transaction using the
        # same prepare_timestamp and prepared_id as the leader.  Leave it active so there is a
        # live prepared DELETE present when step-up fires in Phase 3.
        conn_follow = self._open_follower(checkpoint_meta)
        session_f = conn_follow.open_session()
        follower_cursors = [session_f.open_cursor(uri) for uri in uris]
        session_f.begin_transaction()
        for c in follower_cursors:
            for i in range(1, 4):
                c.set_key(i)
                self.assertEqual(0, c.remove())
        session_f.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id='      + self.prepared_id_str(789))

        # ---- Phase 3 (Step-up while prepare is live) ----
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # ---- Phase 4 (Resolve) ----
        self._resolve_prepared(session_f, follower_cursors)

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

        # ---- Verify ----
        read_session = conn_follow.open_session()
        read_cursors = [read_session.open_cursor(uri) for uri in uris]

        # Original values are always visible at ts=60, regardless of resolution.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for rc in read_cursors:
            self.assertEqual(rc[1], 'original_value_1')
            self.assertEqual(rc[2], 'original_value_2')
            self.assertEqual(rc[3], 'original_value_3')
        read_session.rollback_transaction()

        # After resolve: commit makes keys absent; rollback keeps original values.
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        for rc in read_cursors:
            for i in range(1, 4):
                rc.set_key(i)
                if self.commit:
                    self.assertEqual(wiredtiger.WT_NOTFOUND, rc.search())
                else:
                    self.assertEqual(0, rc.search())
                    self.assertEqual(f'original_value_{i}', rc.get_value())
        read_session.rollback_transaction()

        for rc in read_cursors:
            rc.close()
        read_session.close()
        conn_follow.close()
