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
#
# test_prepare45.py
#   Tests that prepared transactions captured unresolved in a leader checkpoint are correctly
#   resolved (committed or rolled back) on follower step-up. Covers seven operation types:
#   - test_prepare_insert: prepared inserts on new keys
#   - test_prepare_update: prepared updates on existing keys
#   - test_prepare_delete: prepared deletes on existing keys
#   - test_prepare_delete_between_values: prepared delete sandwiched between two committed values
#   - test_prepare_multiple_updates_same_key: single prepared transaction writes the same key
#     multiple times, all updates captured unresolved in the checkpoint
#   - test_prepare_not_captured_insert: prepared insert with prepare_ts > stable_ts; prepare not
#     in checkpoint so the key is absent on the follower
#   - test_prepare_not_captured_update: prepared update with prepare_ts > stable_ts; prepare not
#     in checkpoint so the follower sees only the pre-prepare committed value
#   - test_prepare_not_captured_delete: prepared delete with prepare_ts > stable_ts; prepare not
#     in checkpoint so the follower sees the key as still present

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Disaggregated layered tests are not supported with tiered storage")
@disagg_test_class
class test_prepare45(wttest.WiredTigerTestCase):
    tablename = 'test_prepare45'
    uri = 'layered:' + tablename

    resolve_scenarios = [
        ('commit', dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]
    disagg_storages = gen_disagg_storages('test_prepare45', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolve_scenarios)

    conn_base_config = 'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def _open_follower(self, checkpoint_meta):
        """Open a follower and apply the given checkpoint metadata."""
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn_follow

    def _checkpoint(self, conn):
        """Take a checkpoint on the given connection."""
        session = conn.open_session()
        session.checkpoint()
        session.close()

    def _step_up(self, conn_follow):
        """Step up the follower to leader and checkpoint."""
        conn_follow.reconfigure('disaggregated=(role="leader")')
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)

    def test_prepare_insert(self):
        """Prepared inserts on new keys (4-6) while keys 1-3 are already committed."""

        # Phase 1: Commit values at keys 1-3, then prepare inserts at new keys 4-6.
        # The prepare is captured unresolved in the checkpoint (prepare_ts <= stable_ts).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "committed_value_1"
        cursor[2] = "committed_value_2"
        cursor[3] = "committed_value_3"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        cursor[4] = "prepared_value_4"
        cursor[5] = "prepared_value_5"
        cursor[6] = "prepared_value_6"
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2: Follower picks up the checkpoint and replicates the prepare.
        conn_follow = self._open_follower(checkpoint_meta)

        session_f = conn_follow.open_session()
        cursor_f = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        cursor_f[4] = "prepared_value_4"
        cursor_f[5] = "prepared_value_5"
        cursor_f[6] = "prepared_value_6"
        session_f.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                      ',prepared_id=' + self.prepared_id_str(123))
        if self.commit:
            session_f.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                         ",durable_timestamp=" + self.timestamp_str(210))
        else:
            session_f.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor_f.close()
        session_f.close()

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=60: keys 1-3 visible, keys 4-6 not yet inserted.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(read_cursor[1], "committed_value_1")
        self.assertEqual(read_cursor[2], "committed_value_2")
        self.assertEqual(read_cursor[3], "committed_value_3")
        for i in range(4, 7):
            read_cursor.set_key(i)
            self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
        read_session.rollback_transaction()

        # At ts=200: commit  keys 4-6 visible; rollback  keys 4-6 not found.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        for i in range(4, 7):
            read_cursor.set_key(i)
            if self.commit:
                self.assertEqual(0, read_cursor.search())
                self.assertEqual(f'prepared_value_{i}', read_cursor.get_value())
            else:
                self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_update(self):
        """Prepared updates on existing keys 1-3, plus a newer write at ts=220."""

        # Phase 1: Commit original values, then prepare updates on the same keys.
        # The prepare is captured unresolved in the checkpoint (prepare_ts <= stable_ts).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "original_value_1"
        cursor[2] = "original_value_2"
        cursor[3] = "original_value_3"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        cursor[1] = "updated_value_1"
        cursor[2] = "updated_value_2"
        cursor[3] = "updated_value_3"
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2: Follower picks up the checkpoint, replicates the prepare, then commits
        # a newer update at ts=220 to verify it remains visible regardless of how the prepare resolves.
        conn_follow = self._open_follower(checkpoint_meta)

        session_f = conn_follow.open_session()
        cursor_f = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        cursor_f[1] = "updated_value_1"
        cursor_f[2] = "updated_value_2"
        cursor_f[3] = "updated_value_3"
        session_f.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                      ',prepared_id=' + self.prepared_id_str(123))
        if self.commit:
            session_f.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                         ",durable_timestamp=" + self.timestamp_str(210))
        else:
            session_f.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor_f.close()
        session_f.close()

        write_session = conn_follow.open_session()
        write_cursor = write_session.open_cursor(self.uri)
        write_session.begin_transaction()
        write_cursor[1] = "newer_value_1"
        write_cursor[2] = "newer_value_2"
        write_cursor[3] = "newer_value_3"
        write_session.commit_transaction("commit_timestamp=" + self.timestamp_str(220))
        write_cursor.close()
        write_session.close()

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=60: original values visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(read_cursor[1], "original_value_1")
        self.assertEqual(read_cursor[2], "original_value_2")
        self.assertEqual(read_cursor[3], "original_value_3")
        read_session.rollback_transaction()

        # At ts=200: commit  updated values; rollback  original values.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        if self.commit:
            self.assertEqual(read_cursor[1], "updated_value_1")
            self.assertEqual(read_cursor[2], "updated_value_2")
            self.assertEqual(read_cursor[3], "updated_value_3")
        else:
            self.assertEqual(read_cursor[1], "original_value_1")
            self.assertEqual(read_cursor[2], "original_value_2")
            self.assertEqual(read_cursor[3], "original_value_3")
        read_session.rollback_transaction()

        # At ts=220: newer values visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(220))
        self.assertEqual(read_cursor[1], "newer_value_1")
        self.assertEqual(read_cursor[2], "newer_value_2")
        self.assertEqual(read_cursor[3], "newer_value_3")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_delete(self):
        """Prepared deletes on existing keys 1-3."""

        # Phase 1: Commit original values, then prepare deletes on the same keys.
        # The prepare is captured unresolved in the checkpoint (prepare_ts <= stable_ts).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "original_value_1"
        cursor[2] = "original_value_2"
        cursor[3] = "original_value_3"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(0, cursor.remove())
        cursor.set_key(2)
        self.assertEqual(0, cursor.remove())
        cursor.set_key(3)
        self.assertEqual(0, cursor.remove())
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2: Follower picks up the checkpoint and replicates the prepare.
        conn_follow = self._open_follower(checkpoint_meta)

        session_f = conn_follow.open_session()
        cursor_f = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        cursor_f.set_key(1)
        self.assertEqual(0, cursor_f.remove())
        cursor_f.set_key(2)
        self.assertEqual(0, cursor_f.remove())
        cursor_f.set_key(3)
        self.assertEqual(0, cursor_f.remove())
        session_f.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                      ',prepared_id=' + self.prepared_id_str(123))
        if self.commit:
            session_f.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                         ",durable_timestamp=" + self.timestamp_str(210))
        else:
            session_f.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor_f.close()
        session_f.close()

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=60: original values visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(read_cursor[1], "original_value_1")
        self.assertEqual(read_cursor[2], "original_value_2")
        self.assertEqual(read_cursor[3], "original_value_3")
        read_session.rollback_transaction()

        # At ts=200: commit  keys deleted; rollback  original values visible.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        if self.commit:
            for i in range(1, 4):
                read_cursor.set_key(i)
                self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
        else:
            self.assertEqual(read_cursor[1], "original_value_1")
            self.assertEqual(read_cursor[2], "original_value_2")
            self.assertEqual(read_cursor[3], "original_value_3")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_delete_between_values(self):
        """Prepared delete sandwiched between two committed values (value_160 and value_220)."""

        # Phase 1: Commit value_160, prepare a delete, checkpoint with the prepare unresolved.
        # The prepare is left open so the checkpoint captures it (prepare_ts <= stable_ts).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "value_160"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(160))

        # Prepare a delete on the same key. Leave the transaction open so the checkpoint
        # captures it as an unresolved prepare.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(0, cursor.remove())
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(170) +
                                        ',prepared_id=' + self.prepared_id_str(500))

        # Advance stable past the prepare timestamp so the prepare is captured in the checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(200))

        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Roll back the leader's prepared transaction. The checkpoint already captured the
        # prepare, so the follower sees it as unresolved.
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2: Follower picks up the checkpoint, replicates the prepare, then commits
        # value_220 at ts=220 to create the "sandwiched" scenario.
        conn_follow = self._open_follower(checkpoint_meta)

        session_f = conn_follow.open_session()
        prep_cursor = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        prep_cursor.set_key(1)
        self.assertEqual(0, prep_cursor.remove())
        session_f.prepare_transaction('prepare_timestamp=' + self.timestamp_str(170) +
                                      ',prepared_id=' + self.prepared_id_str(500))
        if self.commit:
            session_f.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                         ",durable_timestamp=" + self.timestamp_str(210))
        else:
            session_f.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        prep_cursor.close()
        session_f.close()

        write_session = conn_follow.open_session()
        write_cursor = write_session.open_cursor(self.uri)
        write_session.begin_transaction()
        write_cursor[1] = "value_220"
        write_session.commit_transaction("commit_timestamp=" + self.timestamp_str(220))
        write_cursor.close()
        write_session.close()

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=160: value_160 visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(160))
        self.assertEqual(read_cursor[1], "value_160")
        read_session.rollback_transaction()

        # At ts=200: commit  key deleted; rollback  value_160 still visible.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        if self.commit:
            read_cursor.set_key(1)
            self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
        else:
            self.assertEqual(read_cursor[1], "value_160")
        read_session.rollback_transaction()

        # At ts=220: value_220 visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(220))
        self.assertEqual(read_cursor[1], "value_220")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_multiple_updates_same_key(self):
        """A single prepared transaction writes the same key multiple times. On commit only the
        last write is visible; on rollback the pre-prepare value is restored."""

        # Phase 1 (Leader):
        # - Commit initial value at ts=60.
        # - Within one prepared transaction, write key 1 three times ("value_a", "value_b",
        #   "value_c"). Leave the transaction open so the checkpoint captures it as unresolved.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "original_value"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        # Single prepared transaction with multiple writes to key 1.
        self.session.begin_transaction()
        cursor[1] = "value_a"
        cursor[1] = "value_b"
        cursor[1] = "value_c"
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2 (Follower): Replay the same prepared transaction (same prepared_id, same
        # sequence of writes) and resolve it.
        conn_follow = self._open_follower(checkpoint_meta)

        session_f = conn_follow.open_session()
        cursor_f = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        cursor_f[1] = "value_a"
        cursor_f[1] = "value_b"
        cursor_f[1] = "value_c"
        session_f.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                      ',prepared_id=' + self.prepared_id_str(123))
        if self.commit:
            session_f.commit_transaction("commit_timestamp=" + self.timestamp_str(200) +
                                         ",durable_timestamp=" + self.timestamp_str(210))
        else:
            session_f.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor_f.close()
        session_f.close()

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=60: original value visible regardless of resolution.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(60))
        self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        # At ts=200: commit  last write "value_c" visible; rollback  original value visible.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        if self.commit:
            self.assertEqual(read_cursor[1], "value_c")
        else:
            self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_not_captured_insert(self):
        """Prepared insert whose prepare_ts exceeds stable_ts at checkpoint time is not durable:
        the follower sees the inserted key as absent."""

        # Phase 1 (Leader):
        # - Commit key 1 at ts=20 (durable at checkpoint).
        # - Insert key 2 in a prepared transaction at prepare_ts=100 > stable_ts=50; because the
        #   prepare timestamp exceeds the stable timestamp, the write is not durable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "committed_value"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Prepare an insert on key 2 with prepare_ts=100 > stable_ts=50.
        self.session.begin_transaction()
        cursor[2] = "prepared_value"
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        # Checkpoint at stable=50 (below prepare_ts=100).
        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2 (Follower): the prepare was not durable; no prepare replay is issued.
        conn_follow = self._open_follower(checkpoint_meta)

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=20: key 1 is visible (committed before the checkpoint).
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(20))
        self.assertEqual(read_cursor[1], "committed_value")
        read_session.rollback_transaction()

        # At ts=200: key 2 is absent because the prepare was not durable at checkpoint time.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        read_cursor.set_key(2)
        self.assertEqual(wiredtiger.WT_NOTFOUND, read_cursor.search())
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_not_captured_update(self):
        """Prepared update whose prepare_ts exceeds stable_ts at checkpoint time is not durable:
        the follower sees only the pre-prepare committed value."""

        # Phase 1 (Leader):
        # - Commit key 1 with "original_value" at ts=20 (durable at checkpoint).
        # - Update key 1 in a prepared transaction at prepare_ts=100 > stable_ts=50; because the
        #   prepare timestamp exceeds the stable timestamp, the update is not durable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "original_value"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Prepare an update on key 1 with prepare_ts=100 > stable_ts=50.
        self.session.begin_transaction()
        cursor[1] = "prepared_value"
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        # Checkpoint at stable=50 (below prepare_ts=100).
        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2 (Follower): the prepare was not durable; no prepare replay is issued.
        conn_follow = self._open_follower(checkpoint_meta)

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=20: original value visible (committed before the checkpoint).
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(20))
        self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        # At ts=200: still "original_value" because the update was not durable at checkpoint time.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()

    def test_prepare_not_captured_delete(self):
        """Prepared delete whose prepare_ts exceeds stable_ts at checkpoint time is not durable:
        the follower sees the key as still present."""

        # Phase 1 (Leader):
        # - Commit key 1 with "original_value" at ts=20 (durable at checkpoint).
        # - Delete key 1 in a prepared transaction at prepare_ts=100 > stable_ts=50; because the
        #   prepare timestamp exceeds the stable timestamp, the delete is not durable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = "original_value"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        # Prepare a delete on key 1 with prepare_ts=100 > stable_ts=50.
        self.session.begin_transaction()
        cursor.set_key(1)
        self.assertEqual(0, cursor.remove())
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
                                        ',prepared_id=' + self.prepared_id_str(123))

        # Checkpoint at stable=50 (below prepare_ts=100).
        self._checkpoint(self.conn)

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        self.conn.close("debug=(skip_checkpoint=true)")

        # Phase 2 (Follower): the prepare was not durable; no prepare replay is issued.
        conn_follow = self._open_follower(checkpoint_meta)

        # Phase 3: Step up and verify.
        self._step_up(conn_follow)

        read_session = conn_follow.open_session()
        read_cursor = read_session.open_cursor(self.uri)

        # At ts=20: original value visible (committed before the checkpoint).
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(20))
        self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        # At ts=200: still "original_value" because the delete was not durable at checkpoint time.
        read_session.begin_transaction("read_timestamp=" + self.timestamp_str(200))
        self.assertEqual(read_cursor[1], "original_value")
        read_session.rollback_transaction()

        read_cursor.close()
        read_session.close()
