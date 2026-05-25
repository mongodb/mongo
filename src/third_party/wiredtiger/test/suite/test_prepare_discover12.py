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
# test_prepare_discover12.py
#   A prepared insert that is rolled back on a follower must leave the key
#   absent on the leader once the rollback timestamp becomes stable, even when
#   the carrying page is evicted and re-read. The behavior must hold both when
#   the key receives no further writes and when a newer regular commit lands on
#   the same key before step-up.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover12(wttest.WiredTigerTestCase):
    tablename = 'test_prepare_discover12'
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages('test_prepare_discover12', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    # ---- helpers ----

    def _open_follower(self, checkpoint_meta):
        """Open a follower connection seeded with the given checkpoint."""
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

    def _evict_key(self, conn, key):
        """Force the page holding `key` out of memory."""
        evict_session = conn.open_session('debug=(release_evict_page=true)')
        evict_cursor = evict_session.open_cursor(self.uri)
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        evict_session.close()

    def _create_leader_baseline(self, baseline_key, baseline_value,
        baseline_commit_ts, stable_ts):
        """Create the table on the leader with a single baseline key, take a
        checkpoint, close the leader without writing another checkpoint, and
        return the checkpoint metadata so a follower can be opened on top of
        the same state."""
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable_ts - 20))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(stable_ts - 20))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[baseline_key] = baseline_value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(baseline_commit_ts))
        cursor.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable_ts))
        self._checkpoint(self.conn)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.conn.close('debug=(skip_checkpoint=true)')
        return checkpoint_meta

    def _prepare_then_rollback(self, conn, key, value, prepared_id, prepare_ts, rollback_ts):
        """Perform a prepared INSERT for `key` and roll it back with a rollback
        timestamp greater than the prepare timestamp."""
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor[key] = value
        session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))
        session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(rollback_ts))
        cursor.close()
        session.close()

    def _commit_value(self, conn, key, value, commit_ts):
        """Commit a regular update at the given timestamp."""
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor[key] = value
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()
        session.close()

    def _set_stable_and_checkpoint(self, conn, stable_ts):
        conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable_ts))
        self._checkpoint(conn)

    def _assert_search(self, conn, key, read_ts, expected_value):
        """Read `key` at `read_ts`; expected_value=None means WT_NOTFOUND."""
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor.set_key(key)
        if expected_value is None:
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor[key], expected_value)
        session.rollback_transaction()
        cursor.close()
        session.close()

    # ---- tests ----

    def test_rolled_back_prepare_alone_remains_absent(self):
        """A follower-rolled-back prepared insert for a previously-nonexistent
        key must remain absent on the leader after the rollback timestamp
        becomes stable, including after the carrying page is evicted and
        re-read."""
        checkpoint_meta = self._create_leader_baseline(
            baseline_key=100, baseline_value='baseline',
            baseline_commit_ts=60, stable_ts=70)
        conn = self._open_follower(checkpoint_meta)

        self._prepare_then_rollback(conn, key=1, value='rolled_back_insert',
            prepared_id=77777, prepare_ts=100, rollback_ts=110)

        # Step up; the rolled-back prepare is now reflected on the leader's
        # table state for the original key.
        conn.reconfigure('disaggregated=(role="leader")')

        # Checkpoint while the rollback is recorded but not yet stable, then
        # force the carrying page out of memory so any subsequent read will
        # restore the page from disk.
        self._set_stable_and_checkpoint(conn, stable_ts=105)
        self._evict_key(conn, 1)

        # Advance stable past the rollback timestamp and checkpoint again.
        self._set_stable_and_checkpoint(conn, stable_ts=200)

        # Reading any time after the rollback must report the key as absent;
        # the baseline must remain intact.
        self._assert_search(conn, key=1, read_ts=220, expected_value=None)
        self._assert_search(conn, key=100, read_ts=220, expected_value='baseline')

        conn.close()

    def test_rolled_back_prepare_with_newer_commit_on_same_key(self):
        """When a follower-rolled-back prepared insert is followed by a regular
        commit on the same key before step-up, the rollback must still be
        respected at timestamps after the rollback timestamp and the newer
        committed value must be visible at and after its commit timestamp.
        The key state must remain correct even when the stable timestamp falls
        between the rollback timestamp and the newer commit timestamp at
        checkpoint time and the carrying page is then evicted."""
        checkpoint_meta = self._create_leader_baseline(
            baseline_key=100, baseline_value='baseline',
            baseline_commit_ts=60, stable_ts=70)
        conn = self._open_follower(checkpoint_meta)

        self._prepare_then_rollback(conn, key=1, value='rolled_back_insert',
            prepared_id=88888, prepare_ts=100, rollback_ts=110)
        self._commit_value(conn, key=1, value='newer_committed_value', commit_ts=150)

        # Step up; both the rolled-back prepare and the newer commit are now
        # on the leader's table state for the same key.
        conn.reconfigure('disaggregated=(role="leader")')

        # Checkpoint with a stable timestamp that is past the rollback but
        # before the newer commit, then evict the carrying page.
        self._set_stable_and_checkpoint(conn, stable_ts=120)
        self._evict_key(conn, 1)

        # Advance stable past the newer commit and checkpoint.
        self._set_stable_and_checkpoint(conn, stable_ts=200)

        # At a read time between the rollback and the newer commit, the key
        # must be absent: the rollback is visible, the newer commit is not.
        self._assert_search(conn, key=1, read_ts=120, expected_value=None)

        # At a read time after the newer commit, the newer value must be
        # visible; the baseline remains intact.
        self._assert_search(conn, key=1, read_ts=160, expected_value='newer_committed_value')
        self._assert_search(conn, key=100, read_ts=160, expected_value='baseline')

        conn.close()
