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

# test_layered89.py
#   Test that cursor.next(), cursor.prev(), and cursor.search() on a follower
#   return committed values and do not raise WT_PREPARE_CONFLICT when the primary
#   has checkpointed a prepared (uncommitted) transaction.
#
#   Setup: the primary and follower both commit initial values, then prepare the same
#   update (same prepared_id, simulating oplog replay). The primary checkpoints with
#   preserve_prepared=true so the snapshot includes the pending update. The follower
#   advances its checkpoint to pick up the primary's snapshot, then rolls back its
#   own copy of the prepare.
#
#   Expected: all cursor operations return the committed values without error.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered89(wttest.WiredTigerTestCase):
    tablename = 'test_layered89'
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages('test_layered89', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = ',create,statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setup_primary_and_follower(self, all_keys, commit_ts=10):
        """
        Open a follower, commit initial values for all_keys on both the primary and
        follower (simulating oplog replay), then checkpoint both so the committed values
        are in the snapshot before any prepare is introduced.

        Returns (conn_follow, session_follow).
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config +
            'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, 'key_format=i,value_format=S')
        conn_follow.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(self.uri)
        for key in all_keys:
            self.session.begin_transaction()
            cursor[key] = 'committed_' + str(key)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

        # Replay the same committed writes on the follower (simulating oplog replay).
        cursor_follow = session_follow.open_cursor(self.uri)
        for key in all_keys:
            session_follow.begin_transaction()
            cursor_follow[key] = 'committed_' + str(key)
            session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor_follow.close()

        # Checkpoint the committed writes before introducing the prepare so that when the
        # primary later checkpoints with the prepare active, the committed values remain
        # readable on the follower after the prepare is rolled back.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(commit_ts))
        self.session.checkpoint()
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(commit_ts))
        self.disagg_advance_checkpoint(conn_follow)

        return conn_follow, session_follow

    def checkpoint_with_prepare(self, conn_follow, prepare_ts):
        """
        Checkpoint the primary with the prepare still active (preserve_prepared=true),
        then advance the follower checkpoint to pick up the primary's snapshot.
        """
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(prepare_ts))
        self.session.checkpoint()

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(prepare_ts))
        self.disagg_advance_checkpoint(conn_follow)

    def prepare_on_conn(self, conn, keys, prepare_ts, delete=False):
        """Open a session on conn, prepare updates (or deletes) for keys, and return the session."""
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        for key in keys:
            if delete:
                cursor.set_key(key)
                cursor.remove()
            else:
                cursor[key] = 'prepared_' + str(key)
        session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor.close()
        return session

    def setup_with_prepare(self, all_keys, prepare_keys, commit_ts=10, prepare_ts=20, delete=False):
        """
        Both the primary and follower commit initial values for all_keys, then prepare
        the same updates (or deletes if delete=True) for prepare_keys using the same
        prepared_id (simulating oplog replay). The primary checkpoints with the prepare
        still active (preserve_prepared=true) so the snapshot includes the pending update.
        The follower advances its checkpoint to pick up the primary's snapshot.

        Returns (conn_follow, session_follow, prepare_session_primary, prepare_session_follow).
        The caller must rollback prepare_session_follow before walking the cursor, then call
        resolve_prepared(prepare_session_primary) for cleanup.
        """
        conn_follow, session_follow = self.setup_primary_and_follower(all_keys, commit_ts)

        prepare_session_primary = self.prepare_on_conn(self.conn, prepare_keys, prepare_ts, delete)
        # Replay the same prepared transaction on the follower (simulating oplog replay).
        prepare_session_follow = self.prepare_on_conn(conn_follow, prepare_keys, prepare_ts, delete)

        self.checkpoint_with_prepare(conn_follow, prepare_ts)

        return conn_follow, session_follow, prepare_session_primary, prepare_session_follow

    def evict_page(self, session, key):
        """Force the follower to reload data from its checkpoint on the next cursor access."""
        evict_session = session.connection.open_session('debug=(release_evict_page)')
        evict_cursor = evict_session.open_cursor(self.uri)
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.close()
        evict_session.close()

    def collect_keys(self, session, forward=True):
        """Walk the cursor and return all visible keys. forward=True uses next(), False uses prev()."""
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        step = cursor.next if forward else cursor.prev
        keys = []
        while step() != wiredtiger.WT_NOTFOUND:
            keys.append(cursor.get_key())
        session.rollback_transaction()
        cursor.close()
        return keys

    def resolve_prepared(self, prepare_session, rollback_ts):
        """Roll back a prepared transaction and close its session."""
        prepare_session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(rollback_ts))
        prepare_session.close()

    def run_walk_test(self, all_keys, prepare_keys, walk_func, delete=False):
        """
        Common harness for cursor-walk tests on a follower whose checkpoint contains
        prepared updates (or tombstones). Returns the keys returned by walk_func.
        """
        conn_follow, session_follow, prepare_session_primary, prepare_session_follow = \
            self.setup_with_prepare(all_keys, prepare_keys, delete=delete)

        # Roll back the follower's prepared transaction so the committed value
        # is what the cursor returns on the follower.
        self.resolve_prepared(prepare_session_follow, rollback_ts=30)

        # Force the follower to reload the pages with prepared cells from the checkpoint.
        # Without eviction the cursor would read the in-memory update chain (which already
        # reflects the rollback), bypassing the on-disk prepared cells we need to exercise.
        for key in prepare_keys:
            self.evict_page(session_follow, key)

        keys = walk_func(session_follow)

        session_follow.close()
        conn_follow.close()

        # Roll back the primary's prepare and take a clean checkpoint so that test teardown
        # (which verifies the table) does not encounter a dangling prepared transaction.
        # stable_timestamp must be advanced past the prepare_timestamp first so the
        # checkpoint can move past the prepared update.
        self.resolve_prepared(prepare_session_primary, rollback_ts=30)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()

        return keys

    def test_next_walk_prepared_update(self):
        """
        cursor.next() on a follower must return all committed keys without
        WT_PREPARE_CONFLICT when the primary has checkpointed a prepared update
        for a subset of keys.
        """
        all_keys = [1, 2, 3, 4, 5]
        keys = self.run_walk_test(all_keys, prepare_keys=[2, 4],
            walk_func=lambda s: self.collect_keys(s, forward=True))
        self.assertEqual(sorted(keys), all_keys)

    def test_prev_walk_prepared_update(self):
        """
        cursor.prev() on a follower must return all committed keys without
        WT_PREPARE_CONFLICT when the primary has checkpointed a prepared update
        for a subset of keys.
        """
        all_keys = [1, 2, 3, 4, 5]
        keys = self.run_walk_test(all_keys, prepare_keys=[2, 4],
            walk_func=lambda s: self.collect_keys(s, forward=False))
        self.assertEqual(sorted(keys), all_keys)

    def test_next_walk_prepared_tombstone(self):
        """
        cursor.next() on a follower must return all committed keys without
        WT_PREPARE_CONFLICT when the primary has checkpointed a prepared delete
        for a subset of keys. The uncommitted delete must be invisible, so all
        keys must still be returned.
        """
        all_keys = [1, 2, 3, 4, 5]
        keys = self.run_walk_test(all_keys, prepare_keys=[2, 4],
            walk_func=lambda s: self.collect_keys(s, forward=True), delete=True)
        self.assertEqual(sorted(keys), all_keys)

    def test_prev_walk_prepared_tombstone(self):
        """
        cursor.prev() on a follower must return all committed keys without
        WT_PREPARE_CONFLICT when the primary has checkpointed a prepared delete
        for a subset of keys. The uncommitted delete must be invisible, so all
        keys must still be returned.
        """
        all_keys = [1, 2, 3, 4, 5]
        keys = self.run_walk_test(all_keys, prepare_keys=[2, 4],
            walk_func=lambda s: self.collect_keys(s, forward=False), delete=True)
        self.assertEqual(sorted(keys), all_keys)

    def test_search_and_search_near_prepared_update(self):
        """
        cursor.search() and cursor.search_near() on a follower must both return the
        committed value without WT_PREPARE_CONFLICT when the primary has checkpointed
        a prepared update for that key.
        """
        all_keys = [1, 2, 3]
        prepare_keys = [2]

        conn_follow, session_follow, prepare_session_primary, prepare_session_follow = \
            self.setup_with_prepare(all_keys, prepare_keys)

        # Roll back the follower's prepared transaction.
        self.resolve_prepared(prepare_session_follow, rollback_ts=30)

        # Evict the page for the prepared key so the cursor reads from the on-disk checkpoint.
        self.evict_page(session_follow, 2)

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction()

        # Key 1: no prepared update; both lookups return the committed value.
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'committed_1')
        cursor.set_key(1)
        self.assertEqual(cursor.search_near(), 0)
        self.assertEqual(cursor.get_value(), 'committed_1')

        # Key 2: primary checkpointed a prepared update; follower rolled back its copy.
        # Both lookups must return the committed value without WT_PREPARE_CONFLICT.
        cursor.set_key(2)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'committed_2')
        cursor.set_key(2)
        self.assertEqual(cursor.search_near(), 0)
        self.assertEqual(cursor.get_value(), 'committed_2')

        session_follow.rollback_transaction()
        cursor.close()

        session_follow.close()
        conn_follow.close()
        # Roll back the primary's prepare and take a clean checkpoint so that test teardown
        # (which verifies the table) does not encounter a dangling prepared transaction.
        # stable_timestamp must be advanced past the prepare_timestamp first so the
        # checkpoint can move past the prepared update.
        self.resolve_prepared(prepare_session_primary, rollback_ts=30)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()
