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
# test_layered84.py
#   Test layered cursor walks on a follower with an advanced checkpoint, exercising the
#   two-cursor merge path. Verifies correct behavior when:
#   - An overwrite update positions only the ingest cursor, then next() forces the stable
#     cursor to open.
#   - A prepared conflict occurs mid-walk and the cursor retries after the prepare resolves.

import os
import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import WiredTigerError, wiredtiger_strerror, WT_PREPARE_CONFLICT
from wtscenario import make_scenarios

@disagg_test_class
class test_layered84(wttest.WiredTigerTestCase):
    tablename = 'test_layered84'
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages('test_layered84', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = ',create,statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def early_setup(self):
        os.mkdir('follower')
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def is_prepare_conflict(self, e):
        return wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e)

    def populate_leader_and_checkpoint(self, keys):
        """Insert data on the leader and checkpoint so the follower's stable table has data."""
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        for key in keys:
            self.session.begin_transaction()
            cursor[key] = f'value_{key}'
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(10 + key))
        cursor.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

    def open_follower(self):
        """Open a follower connection and advance its checkpoint so it sees the leader's data."""
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config +
            'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, 'key_format=i,value_format=S')
        self.disagg_advance_checkpoint(conn_follow)
        return conn_follow, session_follow

    def walk_next_collect(self, cursor):
        """Walk forward collecting keys, stopping on prepared conflict or end-of-table."""
        keys = []
        got_conflict = False
        while True:
            try:
                ret = cursor.next()
            except WiredTigerError as e:
                if self.is_prepare_conflict(e):
                    got_conflict = True
                    break
                raise
            if ret == wiredtiger.WT_NOTFOUND:
                break
            keys.append(cursor.get_key())
        return keys, got_conflict

    def walk_prev_collect(self, cursor):
        """Walk backward collecting keys, stopping on prepared conflict or end-of-table."""
        keys = []
        got_conflict = False
        while True:
            try:
                ret = cursor.prev()
            except WiredTigerError as e:
                if self.is_prepare_conflict(e):
                    got_conflict = True
                    break
                raise
            if ret == wiredtiger.WT_NOTFOUND:
                break
            keys.append(cursor.get_key())
        return keys, got_conflict

    def setup_committed_then_prepared(self, all_keys, committed_keys, prepared_key):
        """
        Set up a follower with committed writes on committed_keys and a prepared write
        on prepared_key. Returns (conn_follow, session_follow, cursor, prepare_session,
        prepare_cursor) with the main session's transaction already begun at read ts 60.
        """
        self.populate_leader_and_checkpoint(all_keys)
        conn_follow, session_follow = self.open_follower()

        ingest_cursor = session_follow.open_cursor(self.uri)
        for key in committed_keys:
            session_follow.begin_transaction()
            ingest_cursor[key] = f'committed_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        ingest_cursor.close()

        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[prepared_key] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        return conn_follow, session_follow, cursor, prepare_session, prepare_cursor

    def commit_prepared(self, prepare_session, prepare_cursor):
        """Commit a prepared transaction at timestamp 60."""
        prepare_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(60) +
            ',durable_timestamp=' + self.timestamp_str(60))
        prepare_session.commit_transaction()
        prepare_cursor.close()
        prepare_session.close()

    def rollback_prepared(self, prepare_session, prepare_cursor):
        """Roll back a prepared transaction."""
        prepare_session.rollback_transaction()
        prepare_cursor.close()
        prepare_session.close()

    def setup_prepared_rollback(self, committed_keys, prepared_key):
        """
        Set up a follower with committed writes on committed_keys and a prepared write on
        prepared_key. prepared_key must not be in committed_keys  it has no committed state,
        so after rollback it must not appear in any scan.
        Returns (conn_follow, session_follow, cursor, prepare_session, prepare_cursor)
        with the main session's transaction begun at read_timestamp=60.
        """
        self.populate_leader_and_checkpoint(committed_keys)
        conn_follow, session_follow = self.open_follower()

        write_cursor = session_follow.open_cursor(self.uri)
        for key in committed_keys:
            session_follow.begin_transaction()
            write_cursor[key] = f'committed_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        write_cursor.close()

        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[prepared_key] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        return conn_follow, session_follow, cursor, prepare_session, prepare_cursor

    def test_overwrite_update_then_next_on_follower(self):
        """
        On a follower, search on a key, overwrite-update it in the same transaction, then
        advance the checkpoint and call next(). The walk must return only the keys that
        sort after the searched key  no keys before the position, no duplicates.
        """
        all_keys = [1, 2, 3, 4, 5]
        self.populate_leader_and_checkpoint(all_keys)
        conn_follow, session_follow = self.open_follower()

        # Search positions the cursor on key 3; the overwrite update keeps it there.
        follow_cursor = session_follow.open_cursor(self.uri, None, 'overwrite=true')
        session_follow.begin_transaction()

        follow_cursor.set_key(3)
        self.assertEqual(follow_cursor.search(), 0)

        # Overwrite update in the same transaction keeps the cursor positioned on key 3.
        follow_cursor.set_key(3)
        follow_cursor.set_value('updated_3')
        follow_cursor.update()
        session_follow.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(25))

        # Advance the checkpoint so the follower sees the latest leader data.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # next() must resume from key 3 and return only higher keys.
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        keys = []
        while follow_cursor.next() != wiredtiger.WT_NOTFOUND:
            keys.append(follow_cursor.get_key())
        session_follow.rollback_transaction()

        # After positioned at key 3, next() should return the remaining keys.
        self.assertTrue(len(keys) > 0, "next() should return keys after the positioned key")
        for key in keys:
            self.assertGreater(key, 3, f"next() returned key {key} which is not after position 3")

        follow_cursor.close()
        session_follow.close()
        conn_follow.close()

    def test_next_walk_prepare_conflict_mid_scan(self):
        """
        Forward walk on a follower where the follower has committed writes only on even keys
        and the leader has all keys. A prepared conflict occurs mid-walk. After the conflict
        resolves, every key  including odd keys that exist only in the leader's data  must
        appear exactly once across the full walk.
        """
        all_keys = [1, 2, 3, 4, 5, 6]
        self.populate_leader_and_checkpoint(all_keys)
        conn_follow, session_follow = self.open_follower()

        # Write only even keys on the follower so that odd keys (3, 5) exist only in the
        # leader's data. If the walk drops its position on conflict, odd keys can be skipped.
        ingest_cursor = session_follow.open_cursor(self.uri)
        for key in [2, 4, 6]:
            session_follow.begin_transaction()
            ingest_cursor[key] = f'ingest_value_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        ingest_cursor.close()

        # Prepare an update on key 4  the next follower key after 2.
        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[4] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        # Walk forward  conflict expected at key 4.
        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict during forward walk")

        # Commit the prepared transaction and retry the walk on the same cursor.
        prepare_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(60) +
            ',durable_timestamp=' + self.timestamp_str(60))
        prepare_session.commit_transaction()
        prepare_cursor.close()
        prepare_session.close()

        keys_after, _ = self.walk_next_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        # Key 3 exists only in the leader's data; it must not be skipped by the conflict.
        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_prepare_conflict_mid_scan(self):
        """
        Backward walk on a follower with committed writes on all keys and a prepared
        conflict mid-scan. Every key must appear exactly once across both segments of
        the walk, mirroring the forward test in the reverse direction.
        """
        all_keys = [1, 2, 3, 4, 5]
        self.populate_leader_and_checkpoint(all_keys)
        conn_follow, session_follow = self.open_follower()

        ingest_cursor = session_follow.open_cursor(self.uri)
        for key in all_keys:
            session_follow.begin_transaction()
            ingest_cursor[key] = f'ingest_value_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        ingest_cursor.close()

        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[3] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict during backward walk")

        prepare_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(60) +
            ',durable_timestamp=' + self.timestamp_str(60))
        prepare_session.commit_transaction()
        prepare_cursor.close()
        prepare_session.close()

        keys_after, _ = self.walk_prev_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_committed_keys_then_prepared(self):
        """
        Forward walk where the follower has committed writes on keys 1-3 and a
        prepared write on key 4. The walk must return keys 1-3 before the conflict,
        and all five keys must appear exactly once across the full walk.
        """
        all_keys = [1, 2, 3, 4, 5]
        # committed_keys=[1,2,3] appear before prepared_key=4 in sort order.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [1, 2, 3], 4)

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict after committed keys 1-3")

        # Committed keys 1-3 must all be visible before the conflict at key 4.
        self.assertEqual(set(keys_before), {1, 2, 3},
            f"Expected keys 1-3 before conflict, got {keys_before}")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_next_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        # A complete walk must visit each key exactly once.  A duplicate
        # across the two segments means a key was returned both before and
        # after the conflict, violating the uniqueness contract of a scan.
        for k in keys_after:
            self.assertNotIn(k, set(keys_before),
                f"Key {k} appears twice  each key must appear exactly once in a complete walk")

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_committed_keys_then_prepared(self):
        """
        Backward walk where the follower has committed writes on keys 3-5 and a
        prepared write on key 2. The walk must return keys 5-3 before the conflict,
        and all five keys must appear exactly once across the full walk.
        """
        all_keys = [1, 2, 3, 4, 5]
        # committed_keys=[3,4,5] appear before prepared_key=2 in reverse sort order.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [3, 4, 5], 2)

        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict after committed keys 3-5")

        # Committed keys 5, 4, 3 must all be visible before the conflict at key 2.
        self.assertEqual(set(keys_before), {3, 4, 5},
            f"Expected keys 3-5 before conflict, got {keys_before}")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_prev_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        # A complete walk must visit each key exactly once.  A duplicate
        # across the two segments means a key was returned both before and
        # after the conflict, violating the uniqueness contract of a scan.
        for k in keys_after:
            self.assertNotIn(k, set(keys_before),
                f"Key {k} appears twice  each key must appear exactly once in a complete walk")

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_ingest_only_committed_then_prepared(self):
        """
        Forward walk where committed follower writes introduce keys that do not exist
        in stable, followed by a prepared key that also only exists on the follower.
        All six keys must appear exactly once across the full walk.
        """
        # Stable has only odd keys; even keys and key 6 exist only on the follower.
        stable_keys = [1, 3, 5]
        self.populate_leader_and_checkpoint(stable_keys)
        conn_follow, session_follow = self.open_follower()

        # Commit new even keys  follower-only keys that interleave with stable keys.
        ingest_cursor = session_follow.open_cursor(self.uri)
        for key in [2, 4]:
            session_follow.begin_transaction()
            ingest_cursor[key] = f'committed_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        ingest_cursor.close()

        # Prepare key 6  a follower-only key beyond the end of stable.
        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[6] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict at key 6")

        # The committed follower keys 2 and 4 must appear before the conflict.
        for k in [2, 4]:
            self.assertIn(k, keys_before,
                f"Committed follower key {k} missing before conflict, got {keys_before}")

        prepare_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(60) +
            ',durable_timestamp=' + self.timestamp_str(60))
        prepare_session.commit_transaction()
        prepare_cursor.close()
        prepare_session.close()

        keys_after, _ = self.walk_next_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        # A complete walk must visit each key exactly once.  A duplicate
        # across the two segments means a key was returned both before and
        # after the conflict, violating the uniqueness contract of a scan.
        for k in keys_after:
            self.assertNotIn(k, set(keys_before),
                f"Key {k} appears twice  each key must appear exactly once in a complete walk")

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, {1, 2, 3, 4, 5, 6},
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_conflict_at_start(self):
        """
        Forward walk where the follower's lowest key is prepared and all other keys are
        committed.  The very first next() must raise a prepared conflict with no keys
        returned beforehand.  After the conflict resolves, all five keys must be
        returned without error.  (This test covers conflict recovery when the conflict
        fires before any key is seen; it does not test position preservation, since
        there is no prior position when the conflict fires on the first step.)
        """
        all_keys = [1, 2, 3, 4, 5]
        # prepared_key=1 sorts first, so the conflict fires before any committed key.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [2, 3, 4, 5], 1)

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict on the first next()")
        self.assertEqual(keys_before, [],
            "No key should be returned before the conflict at the lowest key")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_next_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        self.assertEqual(set(keys_after), set(all_keys),
            f"All keys must appear after the conflict resolves: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_conflict_at_end(self):
        """
        Backward walk where the follower's lowest key is prepared and higher keys are
        committed.  The walk must return the committed higher keys first, then raise a
        prepared conflict at the lowest key.  After the conflict resolves, the
        previously-prepared key must appear and no key may be returned twice.
        """
        all_keys = [1, 2, 3, 4, 5]
        # prepared_key=1 sorts last in backward order, so the conflict fires only
        # after all committed keys 2-5 have been returned.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [2, 3, 4, 5], 1)

        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict,
            "Expected prepared conflict at the lowest key after committed keys 2-5")
        self.assertEqual(set(keys_before), {2, 3, 4, 5},
            f"Expected committed keys 2-5 before conflict, got {keys_before}")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_prev_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        # A complete walk must visit each key exactly once.  A duplicate
        # across the two segments means a key was returned both before and
        # after the conflict, violating the uniqueness contract of a scan.
        for k in keys_after:
            self.assertNotIn(k, set(keys_before),
                f"Key {k} appears twice  each key must appear exactly once in a complete walk")

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_conflict_at_end(self):
        """
        Forward walk where lower keys in the follower are committed and the highest
        key is prepared.  The walk must return all committed keys first, then raise a
        prepared conflict at the highest key.  After the conflict resolves, the
        previously-prepared key must appear and no key may be returned twice.
        """
        all_keys = [1, 2, 3, 4, 5]
        # prepared_key=5 sorts last in forward order, so the conflict fires only
        # after all committed keys 1-4 have been returned.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [1, 2, 3, 4], 5)

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict,
            "Expected prepared conflict at the highest key after committed keys 1-4")
        self.assertEqual(set(keys_before), {1, 2, 3, 4},
            f"Expected committed keys 1-4 before conflict, got {keys_before}")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_next_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        # A complete walk must visit each key exactly once.  A duplicate
        # across the two segments means a key was returned both before and
        # after the conflict, violating the uniqueness contract of a scan.
        for k in keys_after:
            self.assertNotIn(k, set(keys_before),
                f"Key {k} appears twice  each key must appear exactly once in a complete walk")

        all_returned = set(keys_before + keys_after)
        self.assertEqual(all_returned, set(all_keys),
            f"Missing keys. Before: {keys_before}, After: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_conflict_at_start(self):
        """
        Backward walk where the follower's highest key is prepared and all other keys
        are committed.  The very first prev() must raise a prepared conflict with no
        keys returned beforehand.  After the conflict resolves, all five keys must be
        returned without error.  (This test covers conflict recovery when the conflict
        fires before any key is seen; it does not test position preservation, since
        there is no prior position when the conflict fires on the first step.)
        """
        all_keys = [1, 2, 3, 4, 5]
        # prepared_key=5 sorts first in backward order, so the conflict fires before
        # any committed key.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, [1, 2, 3, 4], 5)

        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict on the first prev()")
        self.assertEqual(keys_before, [],
            "No key should be returned before the conflict at the highest key")

        self.commit_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_prev_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        self.assertEqual(set(keys_after), set(all_keys),
            f"All keys must appear after the conflict resolves: {keys_after}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_prepare_conflict_first_key(self):
        """
        Forward walk on a follower where the very first next() raises a prepared conflict and
        committed writes exist at all keys. After the conflict resolves, every key must be
        returned without error.  (This test covers conflict recovery when the conflict fires
        before any key is seen; it does not test position preservation, since there is no
        prior position when the conflict fires on the first step.)
        """
        all_keys = [1, 2, 3, 4, 5]
        self.populate_leader_and_checkpoint(all_keys)
        conn_follow, session_follow = self.open_follower()

        # Commit values for all keys on the follower so there is data beyond the prepared key.
        ingest_cursor = session_follow.open_cursor(self.uri)
        for key in all_keys:
            session_follow.begin_transaction()
            ingest_cursor[key] = f'ingest_value_{key}'
            session_follow.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(30 + key))
        ingest_cursor.close()

        # Prepare key 1  the first key in sort order  so the conflict fires immediately.
        prepare_session = conn_follow.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)
        prepare_session.begin_transaction()
        prepare_cursor[1] = 'prepared_value'
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(50) +
            ',prepared_id=' + self.prepared_id_str(1))

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        got_conflict = False
        try:
            cursor.next()
        except WiredTigerError as e:
            if self.is_prepare_conflict(e):
                got_conflict = True
            else:
                raise
        self.assertTrue(got_conflict, "Expected prepared conflict on first next()")

        prepare_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(60) +
            ',durable_timestamp=' + self.timestamp_str(60))
        prepare_session.commit_transaction()
        prepare_cursor.close()
        prepare_session.close()

        keys, _ = self.walk_next_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        self.assertEqual(set(keys), set(all_keys),
            f"Expected all keys after resolve, got {keys}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_prepare_conflict_then_rollback(self):
        """
        Forward walk where a prepared conflict is encountered mid-scan and the prepared
        transaction is then rolled back. Key 3 was never committed, so after rollback it
        must not appear. Keys returned across both segments must be in strictly ascending
        order  a position loss on rollback would cause already-seen keys to repeat,
        breaking the sort order.
        """
        committed_keys = [1, 2, 4, 5]
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_prepared_rollback(committed_keys, prepared_key=3)

        # Walk forward until the conflict on key 3.
        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict during forward walk")

        # Roll back: key 3 is now gone with no committed state.
        self.rollback_prepared(prepare_session, prepare_cursor)

        # Resume the walk from where it stopped; key 3 must not appear.
        keys_after, _ = self.walk_next_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        all_returned = keys_before + keys_after
        self.assertEqual(sorted(all_returned), all_returned,
            f"Keys out of order  cursor position was likely lost on rollback: {all_returned}")
        self.assertEqual(set(all_returned), set(committed_keys),
            f"Rolled-back key appeared or committed key missing: {all_returned}")

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_prepare_conflict_then_rollback(self):
        """
        Backward walk where a prepared conflict is encountered mid-scan and the prepared
        transaction is then rolled back. Key 3 was never committed, so after rollback it
        must not appear. Keys returned across both segments must be in strictly descending
        order  a position loss on rollback would cause already-seen keys to repeat,
        breaking the sort order.
        """
        committed_keys = [1, 2, 4, 5]
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_prepared_rollback(committed_keys, prepared_key=3)

        # Walk backward until the conflict on key 3.
        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict during backward walk")

        # Roll back: key 3 is now gone with no committed state.
        self.rollback_prepared(prepare_session, prepare_cursor)

        # Resume the walk from where it stopped; key 3 must not appear.
        keys_after, _ = self.walk_prev_collect(cursor)

        session_follow.rollback_transaction()
        cursor.close()

        all_returned = keys_before + keys_after
        self.assertEqual(sorted(all_returned, reverse=True), all_returned,
            f"Keys out of order  cursor position was likely lost on rollback: {all_returned}")
        self.assertEqual(set(all_returned), set(committed_keys),
            f"Rolled-back key appeared or committed key missing: {all_returned}")

        session_follow.close()
        conn_follow.close()

    def test_next_walk_overwrite_rollback(self):
        """
        Forward walk where a prepared transaction overwrites a key that already has a
        committed value. After the conflict, the prepared transaction is rolled back.
        The overwritten key must revert to its prior committed value  not disappear
        as it would if it had no prior committed version. No key should repeat and
        sort order must be preserved.
        """
        all_keys = [1, 2, 3, 4, 5]
        # Key 3 has committed value 'committed_3'; the prepared overwrite is then rolled back.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, all_keys, prepared_key=3)

        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict at key 3")

        # Roll back: the overwrite is canceled; key 3 reverts to its committed value.
        self.rollback_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_next_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        all_returned = keys_before + keys_after
        self.assertEqual(sorted(all_returned), all_returned,
            f"Keys out of order after overwrite rollback: {all_returned}")
        self.assertEqual(set(all_returned), set(all_keys),
            "Rolled-back overwrite must not drop the key  it must revert to its prior committed value")

        # Verify key 3 reads back at its committed value, not the rolled-back prepared value.
        verify_cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        verify_cursor.set_key(3)
        self.assertEqual(verify_cursor.search(), 0)
        self.assertEqual(verify_cursor.get_value(), 'committed_3',
            "Rolled-back overwrite must not affect the prior committed value of key 3")
        session_follow.rollback_transaction()
        verify_cursor.close()

        session_follow.close()
        conn_follow.close()

    def test_prev_walk_overwrite_rollback(self):
        """
        Backward walk where a prepared transaction overwrites a key that already has a
        committed value. After the conflict, the prepared transaction is rolled back.
        The overwritten key must revert to its prior committed value. No key should
        repeat and reverse sort order must be preserved.
        """
        all_keys = [1, 2, 3, 4, 5]
        # Key 3 has committed value 'committed_3'; the prepared overwrite is then rolled back.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_committed_then_prepared(all_keys, all_keys, prepared_key=3)

        keys_before, got_conflict = self.walk_prev_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict at key 3")

        # Roll back: the overwrite is canceled; key 3 reverts to its committed value.
        self.rollback_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_prev_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        all_returned = keys_before + keys_after
        self.assertEqual(sorted(all_returned, reverse=True), all_returned,
            f"Keys out of order after overwrite rollback: {all_returned}")
        self.assertEqual(set(all_returned), set(all_keys),
            "Rolled-back overwrite must not drop the key  it must revert to its prior committed value")

        # Verify key 3 reads back at its committed value, not the rolled-back prepared value.
        verify_cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        verify_cursor.set_key(3)
        self.assertEqual(verify_cursor.search(), 0)
        self.assertEqual(verify_cursor.get_value(), 'committed_3',
            "Rolled-back overwrite must not affect the prior committed value of key 3")
        session_follow.rollback_transaction()
        verify_cursor.close()

        session_follow.close()
        conn_follow.close()

    def test_next_walk_rollback_at_first_key(self):
        """
        Forward walk where the very first next() raises a prepared conflict and the
        prepared transaction is then rolled back. The prepared key was never committed,
        so it must not appear after rollback. This is a boundary case: the conflict fires
        before any key is returned, so the resumed walk must return all committed keys
        from the beginning without returning the rolled-back key.
        """
        committed_keys = [2, 3, 4, 5]
        # Key 1 is only prepared (never committed); it must not appear after rollback.
        conn_follow, session_follow, cursor, prepare_session, prepare_cursor = \
            self.setup_prepared_rollback(committed_keys, prepared_key=1)

        # The first next() fires the conflict at key 1  no keys returned beforehand.
        keys_before, got_conflict = self.walk_next_collect(cursor)
        self.assertTrue(got_conflict, "Expected prepared conflict on the first next()")
        self.assertEqual(keys_before, [],
            "No key should be returned before the conflict when it fires on the first step")

        # Roll back: key 1 is now gone with no committed state.
        self.rollback_prepared(prepare_session, prepare_cursor)

        keys_after, _ = self.walk_next_collect(cursor)
        session_follow.rollback_transaction()
        cursor.close()

        self.assertEqual(set(keys_after), set(committed_keys),
            f"All committed keys must appear after rollback; never-committed key must not: {keys_after}")
        self.assertNotIn(1, keys_after, "Rolled-back key 1 must not appear in the scan")

        session_follow.close()
        conn_follow.close()
