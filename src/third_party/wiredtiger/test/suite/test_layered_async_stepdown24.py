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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# A walk blocked on a prepared update must resume exactly where it stopped, even when the blocked
# key resolves onto a key the other constituent shares, or when the walk's own transaction changes
# the table while the walk is blocked. The prepared updates are resolved by rollback: committing a
# transaction that straddles the step-down panics by design.
class BlockedWalkMixin(LayeredStepdownMixin):
    conn_config = 'statistics=(all),precise_checkpoint=true,' \
        'debug_mode=(disagg_stepdown_prepare=true),' \
        'disaggregated=(role="leader",stepdown_write_mirroring=false)'

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]

    # Keys are named by rank in walk order, so one layout serves both directions.
    def key(self, rank):
        return f'key{rank if self.forward else 10 - rank}'

    def step(self, cursor):
        return cursor.next() if self.forward else cursor.prev()

    def step_back(self, cursor):
        return cursor.prev() if self.forward else cursor.next()

    def walk_back(self, cursor):
        actual = []
        while self.step_back(cursor) == 0:
            actual.append((cursor.get_key(), cursor.get_value()))
        return actual

    # Repositioning may read a prepared key the walk has already passed. On each conflict, resolve
    # the next transaction from the list and retry; a conflict with none left fails the test.
    def walk_back_resolving(self, cursor, prepares, expected_conflicts):
        actual = []
        conflicts = 0
        while True:
            try:
                if self.step_back(cursor) != 0:
                    self.assertEqual(conflicts, expected_conflicts)
                    return actual
            except wiredtiger.WiredTigerError as e:
                self.assertIn(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT), str(e))
                self.assertTrue(prepares, 'the walk conflicted with nothing left to resolve')
                conflicts += 1
                self.resolve(prepares.pop(0))
                continue
            actual.append((cursor.get_key(), cursor.get_value()))

    def prepare(self, rank, prepare_ts):
        session = self.conn.open_session()
        cursor = session.open_cursor(self.uri, None, None)
        session.begin_transaction()
        cursor[self.key(rank)] = 'prepared'
        session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(prepare_ts))
        return session

    # Close the session too: verification at teardown needs exclusive access to the table.
    def resolve(self, session):
        session.rollback_transaction()
        session.close()

    def assert_conflict(self, cursor, back=False):
        step = self.step_back if back else self.step
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: step(cursor),
                                   '/conflict with a prepared update/')

    def assert_returns(self, cursor, rank, value):
        self.assertEqual(self.step(cursor), 0)
        self.assertEqual((cursor.get_key(), cursor.get_value()), (self.key(rank), value))

    def rest_of_walk(self, cursor):
        actual = []
        while self.step(cursor) == 0:
            actual.append((cursor.get_key(), cursor.get_value()))
        return actual

@disagg_test_class
class test_layered_async_stepdown24(BlockedWalkMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True), BlockedWalkMixin.directions)

    def test_deleted_alternate_preserves_readable_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        prepared = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v', self.key(2): 'v'}, 20)
        self.remove_at(self.uri, [self.key(1)], 22)

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assertEqual(self.step(cursor), 0)
        before = [(cursor.get_key(), cursor.get_value())]
        self.assertEqual(before, [(self.key(2), 'v')])
        self.assert_conflict(cursor)
        self.resolve(prepared)
        after = self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        cursor.close()
        self.assertEqual(after, [(self.key(3), 'v')])
        self.assertEqual(before + after, [(self.key(2), 'v'), (self.key(3), 'v')])

    def test_reverse_before_first_key_with_unresolved_prepares(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(3): 'v', self.key(4): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepared = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)
        ingest_prepared = self.prepare(2, 22)

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_conflict(cursor)
        self.assert_conflict(cursor, back=True)
        self.resolve(stable_prepared)
        self.resolve(ingest_prepared)
        self.assertEqual(self.walk_back(cursor), [])
        self.session.rollback_transaction()
        cursor.close()

    # Rolling back a prepared ingest insert exposes the stable value at the same key.
    def test_rollback_ingest_insert_onto_stable_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(r): 'v' for r in (1, 3, 4)}, 10)
        self.set_global_ts(1, 10)
        self.set_step_down_ts(15)
        ingest_prepare = self.prepare(3, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v')
        self.assert_conflict(cursor)
        self.resolve(ingest_prepare)
        actual = self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        cursor.close()
        self.assertEqual(actual, [(self.key(3), 'v'), (self.key(4), 'v')])

    # Ingest blocks on rank 3, which stable also holds. Once ingest resolves onto rank 3, the walk
    # has to get stable past it too, and stable blocks on rank 4 while doing so. Each prepared
    # transaction is resolved as the walk reports its conflict, and rank 3 must still be returned.
    def test_tie_then_block_on_other_constituent(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(r): 'v' for r in (1, 3, 4, 5)}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(4, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v', self.key(3): 'v-ingest'}, 18)
        ingest_prepare = self.prepare(3, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        unresolved = [ingest_prepare, stable_prepare]
        actual = []
        while True:
            try:
                if self.step(cursor) != 0:
                    break
            except wiredtiger.WiredTigerError as e:
                self.assertIn(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT), str(e))
                self.assertTrue(unresolved, 'the walk conflicted with no prepared update left')
                self.resolve(unresolved.pop(0))
                continue
            actual.append((cursor.get_key(), cursor.get_value()))
        self.session.rollback_transaction()

        self.assertEqual(unresolved, [], 'the walk did not block on both prepared updates')
        self.assertEqual(actual, [(self.key(1), 'v'), (self.key(2), 'v'),
            (self.key(3), 'v-ingest'), (self.key(4), 'v'), (self.key(5), 'v')])

    # Writes during a stall are visible when the walk resumes, except for keys behind its blocked
    # position; any available rank 2 is returned before the conflict at rank 3.
    def check_write_while_stable_blocked(self, ingest, op, rank, expected):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(r): 'v' for r in ingest}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v')
        if 2 in ingest:
            self.assert_returns(cursor, 2, 'v')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer.set_key(self.key(rank))
        if op == 'insert':
            writer.set_value('new')
            self.assertEqual(writer.insert(), 0)
        else:
            self.assertEqual(writer.remove(), 0)
        writer.close()

        self.assert_conflict(cursor)
        self.resolve(stable_prepare)

        actual = self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        self.assertEqual(actual, [(self.key(r), v) for r, v in expected])

    def test_write_while_stable_blocked_insert_before_blocked(self):
        self.check_write_while_stable_blocked([5], 'insert', 2, [(3, 'v'), (5, 'v')])

    def test_write_while_stable_blocked_insert_after_blocked(self):
        self.check_write_while_stable_blocked(
            [5], 'insert', 4, [(3, 'v'), (4, 'new'), (5, 'v')])

    def test_write_while_stable_blocked_remove_next_ingest(self):
        self.check_write_while_stable_blocked([5], 'remove', 5, [(3, 'v')])

    def test_write_while_stable_blocked_update_next_ingest(self):
        self.check_write_while_stable_blocked([5], 'insert', 5, [(3, 'v'), (5, 'new')])

    def test_write_while_stable_blocked_remove_alternate_key_before_blocked(self):
        self.check_write_while_stable_blocked([2, 5], 'remove', 2, [(3, 'v'), (5, 'v')])

    def test_write_while_stable_blocked_update_alternate_key_before_blocked(self):
        self.check_write_while_stable_blocked(
            [2, 5], 'insert', 2, [(3, 'v'), (5, 'v')])

    # Reversing a walk blocked on a prepared update returns the last key it returned again, as a
    # btree cursor does, then everything before it. Rank 3 is in both constituents, so ingest must
    # still provide its value when it is returned again.
    def test_reverse_onto_shared_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v', self.key(3): 'v-ingest'}, 18)
        ingest_prepare = self.prepare(5, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for rank in (1, 2, 3):
            self.assert_returns(cursor, rank, 'v-ingest' if rank == 3 else 'v')
        self.assert_conflict(cursor)
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(ingest_prepare)
        self.assertEqual(actual,
            [(self.key(3), 'v-ingest'), (self.key(2), 'v'), (self.key(1), 'v')])

    # The walk's own transaction writes while the walk is blocked, then the walk reverses: it still
    # returns the last returned key again first, not the key the other constituent is on.
    def test_reverse_after_context_change(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v', self.key(6): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(6, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v', self.key(4): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for rank in (1, 2, 3, 4):
            self.assert_returns(cursor, rank, 'v')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer[self.key(9)] = 'new'
        writer.close()

        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(4), 'v'), (self.key(3), 'v'),
            (self.key(2), 'v'), (self.key(1), 'v')])

    # The walk's last returned key gets a prepared update, visible under a new read context, while
    # the other constituent is blocked on a prepared update. Reversing reads that key again and hits
    # a prepare conflict on it too. The walk keeps reporting the prepare conflict, and once both
    # prepared transactions are resolved returns that key again, then everything before it.
    def test_reverse_when_last_key_becomes_prepared(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(r): 'v' for r in (1, 2, 3, 4)}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v-ingest'}, 18)

        # Rank 2 is in both constituents, so stepping stable off it blocks on the prepared rank 3.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        self.assert_returns(cursor, 1, 'v')
        self.assert_returns(cursor, 2, 'v-ingest')
        self.assert_conflict(cursor)

        ingest_prepare = self.prepare(2, 22)
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_conflict(cursor, back=True)
        self.assert_conflict(cursor, back=True)

        self.resolve(stable_prepare)
        self.resolve(ingest_prepare)
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.assertEqual(actual, [(self.key(2), 'v-ingest'), (self.key(1), 'v')])

    # As above, but after the reversal hits a prepare conflict on the last returned key the walk
    # turns back again. Like a btree cursor stepping on from a key it hit a prepare conflict on, it
    # resumes past that key: the keys before it, which the other constituent holds, are not
    # returned again.
    def test_resume_after_failed_reversal(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(r): 'v' for r in (2, 3, 4, 5)}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(4, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v-ingest', self.key(3): 'v-ingest'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        for rank in (1, 2, 3):
            self.assert_returns(cursor, rank, 'v-ingest' if rank in (1, 3) else 'v')
        self.assert_conflict(cursor)

        ingest_prepare = self.prepare(3, 22)
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_conflict(cursor, back=True)
        self.assert_conflict(cursor)

        self.resolve(stable_prepare)
        self.resolve(ingest_prepare)
        actual = self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        self.assertEqual(actual, [(self.key(4), 'v'), (self.key(5), 'v')])

    # A new walk blocks on both constituents, and ingest holds the stopped key. Reversing steps
    # ingest back, then conflicts when stable steps back onto a key that another transaction
    # prepared in the meantime. Once that key, and any stable key the reversal reads behind it, are
    # resolved the reversal completes, although the stopped key is still prepared in ingest: a
    # retry must not read it again.
    def test_reverse_retry_after_partial_progress(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)

        # A transaction that began before the step-down inserts into stable, and is prepared later.
        late_session = self.conn.open_session()
        late_cursor = late_session.open_cursor(self.uri, None, None)
        late_session.begin_transaction()
        late_cursor[self.key(1)] = 'late'

        self.set_step_down_ts(15)
        ingest_prepare = self.prepare(2, 22)

        # Neither prepared key is the first key the walk sees: the late insert is not visible yet.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_conflict(cursor)

        late_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(45))
        self.assert_conflict(cursor, back=True)

        late_session.rollback_transaction()
        prepares = [stable_prepare]
        actual = self.walk_back_resolving(cursor, prepares, expected_conflicts=1)
        self.session.rollback_transaction()
        cursor.close()
        late_cursor.close()
        late_session.close()
        for session in prepares:
            self.resolve(session)
        self.resolve(ingest_prepare)
        self.assertEqual(actual, [])

    # A reversal moves ingest back onto the last returned key, then conflicts on stable, which has a
    # key that another transaction prepared in the meantime. Before the retry the walk's own
    # transaction updates the last returned key. The retry does not return the value ingest held
    # before the update.
    def test_reverse_retry_after_context_change(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(5): 'v'}, 10)
        self.set_global_ts(1, 10)

        # A transaction that began before the step-down inserts into stable, and is prepared later.
        late_session = self.conn.open_session()
        late_cursor = late_session.open_cursor(self.uri, None, None)
        late_session.begin_transaction()
        late_cursor[self.key(3)] = 'late'

        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v-ingest', self.key(2): 'v-ingest'}, 18)
        ingest_prepare = self.prepare(4, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v-ingest')
        self.assert_returns(cursor, 2, 'v-ingest')
        self.assert_conflict(cursor)

        late_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(45))
        self.assert_conflict(cursor, back=True)

        writer = self.session.open_cursor(self.uri, None, None)
        writer[self.key(2)] = 'new'
        writer.close()
        late_session.rollback_transaction()
        self.resolve(ingest_prepare)
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        cursor.close()
        late_cursor.close()
        late_session.close()
        self.assertEqual(actual, [(self.key(2), 'new'), (self.key(1), 'v-ingest')])

    # As above, but the last returned key is in stable, which returned it before the reversal conflicts
    # twice: stable steps back onto a key another transaction prepared, and the walk's own write
    # to an ingest key changes the context before the retry. The retry returns the last returned key
    # from stable again, and the ingest key with the value written.
    def test_reverse_retry_after_context_change_on_returned_stable_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(2): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(6, 12)
        late_session = self.conn.open_session()
        late_cursor = late_session.open_cursor(self.uri, None, None)
        late_session.begin_transaction()
        late_cursor[self.key(3)] = 'late'
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'vi'}, 18)
        ingest_prepare = self.prepare(5, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'vi')
        self.assert_returns(cursor, 2, 'v')
        self.assert_conflict(cursor)

        late_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(45))
        self.assert_conflict(cursor, back=True)

        writer = self.session.open_cursor(self.uri, None, None)
        writer[self.key(1)] = 'new'
        writer.close()
        self.assert_conflict(cursor, back=True)
        late_session.rollback_transaction()
        prepares = [ingest_prepare, stable_prepare]
        actual = self.walk_back_resolving(cursor, prepares,
                                         expected_conflicts=2 if self.forward else 0)
        self.session.rollback_transaction()
        cursor.close()
        late_cursor.close()
        late_session.close()
        for session in prepares:
            self.resolve(session)
        self.assertEqual(actual, [(self.key(2), 'v'), (self.key(1), 'new')])

    # The walk returns a key both constituents hold, then blocks on a prepared update stepping the
    # other constituent past it. The key then becomes deleted, so ingest holds a tombstone over the
    # stable copy. Reversing does not return the deleted key, and returns the key before it.
    def deleted_last_key_setup(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(r): 'v' for r in (1, 2, 3, 4)}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v-ingest'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        self.assert_returns(cursor, 1, 'v')
        self.assert_returns(cursor, 2, 'v-ingest')
        self.assert_conflict(cursor)
        return cursor, stable_prepare

    # The walk's own transaction deletes the last returned key through another cursor.
    def test_reverse_after_last_key_deleted_by_walk(self):
        cursor, stable_prepare = self.deleted_last_key_setup()
        writer = self.session.open_cursor(self.uri, None, None)
        writer.set_key(self.key(2))
        self.assertEqual(writer.remove(), 0)
        writer.close()
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'v')])

    # Another transaction deletes the last returned key, and the walk reads under a new snapshot.
    def test_reverse_after_last_key_deleted_by_other_transaction(self):
        cursor, stable_prepare = self.deleted_last_key_setup()
        self.session.commit_transaction()
        self.remove_at(self.uri, [self.key(2)], 22)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'v')])

    # The walk returns a key both constituents hold, then blocks stepping the other constituent past
    # it. Reversing returns that key again, as a btree cursor does.
    def test_reverse_after_alternate_block(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(2): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(2, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v-ingest'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v-ingest')
        self.assert_conflict(cursor)
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'v-ingest')])

    # The walk reverses out of a stall onto the key it last returned, then turns back and blocks
    # again at once. Each reversal returns that key again, as a btree cursor does.
    def test_reverse_twice_around_block(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(2): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(1, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(4): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for rank in (4, 3, 2):
            self.assertEqual(self.step_back(cursor), 0)
            self.assertEqual(cursor.get_key(), self.key(rank))
        for _ in range(2):
            self.assert_conflict(cursor, back=True)
            self.assert_returns(cursor, 2, 'v')
        actual = self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(3), 'v'), (self.key(4), 'v')])

    # The walk's own transaction inserts a key between the last key returned and the blocked one,
    # on the other constituent, then the walk reverses. As with a btree cursor stepping back from
    # the blocked key, that key is returned first.
    def test_reverse_after_insert_before_blocked(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer[self.key(2)] = 'new'
        writer.close()

        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(2), 'new'), (self.key(1), 'v')])

    # The walk's own transaction inserts a key past the blocked one, before the key the other
    # constituent is on, then the walk reverses. As with a btree cursor, that key is not returned.
    def test_reverse_after_insert_past_blocked(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer[self.key(4)] = 'new'
        writer.close()

        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'v')])

    # A walk positioned by a search passes a prepared ingest key it never read. It blocks on a
    # stable prepared update, then reverses into that ingest key. The walk keeps reporting the
    # conflict until the ingest key resolves, then, like a btree cursor reversing out of a stall,
    # returns the key it last returned again and continues below it.
    def test_reverse_into_second_conflict(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v', self.key(5): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(5, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)
        ingest_prepare = self.prepare(2, 22)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        cursor.set_key(self.key(3))
        self.assertEqual(cursor.search(), 0)
        self.assert_conflict(cursor)
        self.assert_conflict(cursor, back=True)
        self.assert_conflict(cursor, back=True)
        self.resolve(ingest_prepare)
        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(3), 'v'), (self.key(1), 'v')])

    # A walk blocked on a prepared update in stable reverses across a step-down. The walk is
    # positioned on the last key it returned, now on a follower. The next operation completes the
    # step-down on that positioned cursor, including a write through it.
    def test_write_after_reversal_across_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v', self.key(5): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(5, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v', self.key(6): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for rank in (1, 2, 3):
            self.assert_returns(cursor, rank, 'v')
        self.assert_conflict(cursor)

        self.complete_step_down(15)
        self.assertEqual(self.step_back(cursor), 0)
        self.assertEqual(cursor.get_key(), self.key(3))
        cursor.set_value('new')
        self.assertEqual(cursor.update(), 0)
        read_cursor = self.session.open_cursor(self.uri)
        read_cursor.set_key(self.key(3))
        self.assertEqual(read_cursor.search(), 0)
        self.assertEqual((read_cursor.get_key(), read_cursor.get_value()), (self.key(3), 'new'))
        read_cursor.close()
        self.session.rollback_transaction()
        cursor.close()
        try:
            stable_prepare.rollback_transaction()
        except wiredtiger.WiredTigerError:
            self.assertEqual(stable_prepare.get_last_error()[1], wiredtiger.WT_CONFLICT_DISAGG)
        stable_prepare.close()

    # The walk returns an ingest key while stable is already blocked on a later prepared key. After
    # the step-down, the walk continues or reverses from the returned key without skipping stable.
    def check_step_down_after_ingest_key(self, reverse):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(1): 'v', self.key(3): 'v', self.key(5): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(5, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v', self.key(4): 'v', self.key(6): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for rank in (1, 2, 3, 4):
            self.assert_returns(cursor, rank, 'v')

        self.complete_step_down(15)
        actual = self.walk_back(cursor) if reverse else self.rest_of_walk(cursor)
        self.session.rollback_transaction()
        cursor.close()
        try:
            stable_prepare.rollback_transaction()
        except wiredtiger.WiredTigerError:
            self.assertEqual(stable_prepare.get_last_error()[1], wiredtiger.WT_CONFLICT_DISAGG)
        stable_prepare.close()
        expected = (3, 2, 1) if reverse else (5, 6)
        self.assertEqual(actual, [(self.key(r), 'v') for r in expected])

    def test_continue_after_step_down_on_ingest_key(self):
        self.check_step_down_after_ingest_key(reverse=False)

    def test_reverse_after_step_down_on_ingest_key(self):
        self.check_step_down_after_ingest_key(reverse=True)

    # A new walk blocks on its first key, on stable or on both constituents, and retries. Like a
    # btree cursor blocked there, reversing returns only keys before that key in the walk
    # direction: none, or a key the walk's own transaction inserted there.
    def check_reverse_from_first_key(self, ingest_blocked, insert):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(3): 'v', self.key(4): 'v', self.key(5): 'v'}, 10)
        self.set_global_ts(1, 10)
        prepares = [self.prepare(3, 12)]
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)
        if ingest_blocked:
            prepares.append(self.prepare(2, 22))

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_conflict(cursor)
        self.assert_conflict(cursor)
        if insert:
            writer = self.session.open_cursor(self.uri, None, None)
            writer[self.key(1)] = 'new'
            writer.close()
        actual = self.walk_back_resolving(cursor, prepares,
                                         expected_conflicts=1 if ingest_blocked else 0)
        self.session.rollback_transaction()
        for session in prepares:
            self.resolve(session)
        self.assertEqual(actual, [(self.key(1), 'new')] if insert else [])

    def test_reverse_from_first_key_stable_blocked(self):
        self.check_reverse_from_first_key(ingest_blocked=False, insert=False)

    def test_reverse_from_first_key_stable_blocked_insert(self):
        self.check_reverse_from_first_key(ingest_blocked=False, insert=True)

    def test_reverse_from_first_key_both_blocked(self):
        self.check_reverse_from_first_key(ingest_blocked=True, insert=False)

@disagg_test_class
class test_layered_async_stepdown24_mirrored(BlockedWalkMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    conn_config = BlockedWalkMixin.conn_config.replace(
        'stepdown_write_mirroring=false', 'stepdown_write_mirroring=true')
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True), BlockedWalkMixin.directions)

    # With write mirroring, the walk's own transaction removes the last key returned from stable
    # while the walk is blocked there, leaving nothing before it, and may insert a key past the
    # blocked one. Reversing returns nothing.
    def check_last_key_removed(self, insert_past_blocked):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(2): 'v', self.key(4): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(4, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(6): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 2, 'v')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer.set_key(self.key(2))
        self.assertEqual(writer.remove(), 0)
        if insert_past_blocked:
            writer[self.key(5)] = 'new'
        writer.close()

        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        cursor.close()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [])

        # Verification needs the step-down completed when writes are mirrored.
        self.complete_step_down(15)

    # The walk returns a key both constituents hold, then blocks on a prepared update stepping the
    # other constituent past it. With write mirroring, the walk's own transaction deletes that key
    # from both constituents, so neither holds it. Reversing does not return it.
    def check_last_key_deleted_from_both(self, earlier_key, insert_past_blocked):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        ranks = (1, 2, 3, 4) if earlier_key else (2, 3, 4)
        self.write_at(self.uri, {self.key(r): 'v' for r in ranks}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(3, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(2): 'v-ingest'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        if earlier_key:
            self.assert_returns(cursor, 1, 'v')
        self.assert_returns(cursor, 2, 'v-ingest')
        self.assert_conflict(cursor)

        writer = self.session.open_cursor(self.uri, None, None)
        writer.set_key(self.key(2))
        self.assertEqual(writer.remove(), 0)
        if insert_past_blocked:
            writer[self.key(5)] = 'new'
        writer.close()

        actual = self.walk_back(cursor)
        self.session.rollback_transaction()
        cursor.close()
        self.resolve(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'v')] if earlier_key else [])

        # Verification needs the step-down completed when writes are mirrored.
        self.complete_step_down(15)

    def test_reverse_after_last_key_removed(self):
        self.check_last_key_removed(insert_past_blocked=False)

    def test_reverse_after_last_key_removed_insert_past_blocked(self):
        self.check_last_key_removed(insert_past_blocked=True)

    def test_reverse_after_last_key_deleted_from_both(self):
        self.check_last_key_deleted_from_both(earlier_key=True, insert_past_blocked=False)

    def test_reverse_after_last_key_deleted_from_both_insert_past_blocked(self):
        self.check_last_key_deleted_from_both(earlier_key=True, insert_past_blocked=True)

    def test_reverse_after_last_key_deleted_from_both_nothing_before(self):
        self.check_last_key_deleted_from_both(earlier_key=False, insert_past_blocked=False)

    def test_reverse_after_last_key_deleted_from_both_nothing_before_insert_past_blocked(self):
        self.check_last_key_deleted_from_both(earlier_key=False, insert_past_blocked=True)

    def write_own(self, rank, value):
        writer = self.session.open_cursor(self.uri, None, None)
        if value is None:
            writer.set_key(self.key(rank))
            self.assertEqual(writer.remove(), 0)
        else:
            writer[self.key(rank)] = value
        writer.close()

    def finish(self, stable_prepare):
        self.session.rollback_transaction()
        self.resolve(stable_prepare)
        # Verification needs the step-down completed when writes are mirrored.
        self.complete_step_down(15)

    # The walk returns a key both constituents hold, updates it, then blocks stepping the other
    # constituent past it. Reversing returns the key again with the new value.
    def test_reverse_after_updating_last_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(2): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(2, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.assert_returns(cursor, 1, 'v')
        self.write_own(1, 'new')
        self.assert_conflict(cursor)
        actual = self.walk_back(cursor)
        cursor.close()
        self.finish(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'new')])

    # The walk's own transaction removes a key, a new walk blocks past it, and the transaction
    # inserts the key again. Reversing returns it: a deleted key is not where the walk stopped.
    def test_reverse_onto_reinserted_key(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(2): 'v', self.key(3): 'v'}, 10)
        self.set_global_ts(1, 10)
        stable_prepare = self.prepare(2, 12)
        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(1): 'v'}, 18)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        self.write_own(1, None)
        self.assert_conflict(cursor)
        self.write_own(1, 'new')
        actual = self.walk_back(cursor)
        cursor.close()
        self.finish(stable_prepare)
        self.assertEqual(actual, [(self.key(1), 'new')])

if __name__ == '__main__':
    wttest.run()
