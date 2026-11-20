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
from functools import partial
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# A walk over a layered table tracks a position in each constituent. A prepared update blocks the
# constituent holding it: the walk keeps that position while the key it would return is withheld.
# Both constituents can carry prepared content, so both can block a walk, and either way the walk
# must report the conflict and then resume from where it stopped once the conflict is resolved.
@disagg_test_class
class test_layered_async_stepdown16(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,' \
        'debug_mode=(disagg_stepdown_prepare=true),'

    # Where the blocking prepared update lives. Content written before the step-down timestamp is
    # announced goes to the stable constituent; content written after it goes to ingest.
    prepare_targets = [
        # Prepared transactions that straddle the announcement cannot commit.
        ('prepare_on_stable_rollback', dict(prepare_before_step_down=True, commit_prepared=False)),
        ('prepare_on_ingest_rollback', dict(prepare_before_step_down=False, commit_prepared=False)),
        ('prepare_on_ingest_commit', dict(prepare_before_step_down=False, commit_prepared=True)),
    ]

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]

    # Populate stable with the keys immediately beside the blocked key to exercise ties even
    # without mirroring. With mirroring enabled, ingest writes also create ties on stable.
    tie_scenarios = [
        ('no_tie', dict(with_tie=False)),
        ('with_tie', dict(with_tie=True)),
    ]

    # Whether writes in the step-down window are mirrored to stable. Mirrored, a prepared update
    # written after the announcement is in both constituents, so both block the walk.
    mirroring_modes = [
        ('unmirrored', dict(mirroring=False)),
        ('mirrored', dict(mirroring=True)),
    ]

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader",stepdown_write_mirroring=%s)' % \
            ('true' if self.mirroring else 'false')

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, prepare_targets, directions, tie_scenarios, mirroring_modes)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    nrows = 40

    # The key the prepared update covers, placed in the middle so the walk reaches it with keys
    # both before and after it still to return.
    blocked_index = nrows // 2

    def keys(self):
        return [f'key{i:04d}' for i in range(self.nrows)]

    # Interleaving the two constituents' keys keeps both populated across the whole walk.
    def stable_keys(self):
        return [k for i, k in enumerate(self.keys()) if i % 2 == 0]

    def ingest_keys(self):
        return [k for i, k in enumerate(self.keys()) if i % 2 == 1]

    def set_global_ts(self, oldest, stable):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(oldest) +
                                 ',stable_timestamp=' + self.timestamp_str(stable))

    def write_at(self, uri, items, commit_ts):
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for k, v in items.items():
            cursor[k] = v
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    # Resolve the blocking transaction on the first conflict, then continue on the same cursor.
    def walk_resolving_conflict(self, cursor, resolve):
        step = cursor.next if self.forward else cursor.prev
        seen = []
        conflicts = 0
        resolved = False

        while True:
            try:
                if step() != 0:
                    break
            except wiredtiger.WiredTigerError as e:
                if wiredtiger.wiredtiger_strerror(
                        wiredtiger.WT_PREPARE_CONFLICT) not in str(e):
                    raise
                conflicts += 1
                # Bound the retries so a walk that never makes progress fails as a hang here
                # rather than spinning.
                self.assertLess(conflicts, 10,
                    'the walk never resumed past the prepared key')
                if not resolved:
                    resolve()
                    resolved = True
                continue
            seen.append((cursor.get_key(), cursor.get_value()))

        return seen, conflicts

    def early_setup(self):
        # Scenario copies must not share teardown actions bound to another test instance.
        self.teardown_actions = []

    def finish_step_down_for_teardown(self):
        self.complete_step_down(15)
        return (0, '')

    # Build the table with the prepared update on the blocked key. Returns the blocked key, the
    # prepared session and cursor, and a function that resolves the prepared transaction.
    def prepare_table(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        keys = self.keys()
        blocked_key = keys[self.blocked_index]

        # The keys on either side of the blocked one, tied between both constituents: whichever
        # direction the walk takes, it ties on one of these immediately before the blocked key.
        tie_keys = [keys[self.blocked_index - 1], keys[self.blocked_index + 1]]

        # Populate half the keys and make them stable, so a walk reads them from the stable
        # constituent.
        self.write_at(self.uri, {k: 'v-initial' for k in self.stable_keys()}, 10)
        if self.with_tie:
            self.write_at(self.uri, {k: 'v-stable-original' for k in tie_keys}, 10)
        self.set_global_ts(1, 10)

        # A prepare before the announcement has to sit below it, like every write that precedes a
        # step-down; one after it is inside the window and sits above.
        prepare_ts = 12 if self.prepare_before_step_down else 30
        step_down_ts = 15

        prepare_session = self.conn.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri, None, None)

        def prepare():
            prepare_session.begin_transaction()
            prepare_cursor[blocked_key] = 'v-prepared'
            prepare_session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(prepare_ts))

        if self.prepare_before_step_down:
            prepare()

        # Announce the step-down, then write the remaining keys so they reach the ingest
        # constituent. The walk needs a position in both constituents to consult either. The tie
        # keys are among these, now duplicated on stable above: whichever constituent the walk
        # consults for one of them, the other still holds a copy too.
        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(step_down_ts))
        if self.mirroring:
            self.addTearDownAction(self.finish_step_down_for_teardown)
        self.write_at(self.uri, {k: 'v-initial' for k in self.ingest_keys()}, 20)

        if not self.prepare_before_step_down:
            prepare()

        resolve = partial(prepare_session.commit_transaction,
            'commit_timestamp=' + self.timestamp_str(32) +
            ',durable_timestamp=' + self.timestamp_str(32)) if self.commit_prepared else \
            prepare_session.rollback_transaction
        return blocked_key, prepare_session, prepare_cursor, resolve

    def test_walk_over_prepared_key(self):
        keys = self.keys()
        blocked_key, prepare_session, prepare_cursor, resolve = self.prepare_table()

        # Read above the prepared timestamp so the prepared update is a conflict rather than
        # invisible content the walk may skip, and above the ingest keys so they are all visible.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))

        try:
            seen, conflicts = self.walk_resolving_conflict(cursor, resolve)
        finally:
            self.session.rollback_transaction()
            cursor.close()
            prepare_cursor.close()
            prepare_session.close()

        self.assertEqual(conflicts, 1)

        expected = keys if self.forward else list(reversed(keys))
        self.assertEqual(seen, [(key,
            'v-prepared' if self.commit_prepared and key == blocked_key else 'v-initial')
            for key in expected])

    # A bounded walk whose first key has a prepared update: the btree cursor resets itself on the
    # prepare conflict while it is first positioned, so there is no blocked position to resume
    # from. The walk keeps reporting the prepare conflict, and once the prepared transaction is
    # resolved returns the keys within the bound.
    def test_bounded_walk_starting_on_prepared_key(self):
        keys = self.keys()
        blocked_key, prepare_session, prepare_cursor, resolve = self.prepare_table()

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))
        cursor.set_key(blocked_key)
        cursor.bound('bound=' + ('lower' if self.forward else 'upper'))
        step = cursor.next if self.forward else cursor.prev

        try:
            # Retrying the reset cursor must still conflict while the prepare is unresolved.
            for _ in range(2):
                self.assertRaisesException(wiredtiger.WiredTigerError, step,
                    '/conflict with a prepared update/')
            resolve()
            seen = []
            while step() == 0:
                seen.append((cursor.get_key(), cursor.get_value()))
        finally:
            self.session.rollback_transaction()
            cursor.close()
            prepare_cursor.close()
            prepare_session.close()

        expected = keys[self.blocked_index:] if self.forward else \
            list(reversed(keys[:self.blocked_index + 1]))
        self.assertEqual(seen, [(key,
            'v-prepared' if self.commit_prepared and key == blocked_key else 'v-initial')
            for key in expected])

if __name__ == '__main__':
    wttest.run()
