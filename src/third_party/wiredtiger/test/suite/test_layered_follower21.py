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

# On a disaggregated follower, a transaction at snapshot isolation with no read
# timestamp must observe exactly its snapshot, even when the follower picks up
# a new checkpoint in the middle of the transaction: no data committed after
# the snapshot appears, and no data visible to the snapshot disappears. Adopting
# a checkpoint is deferred while such a reader is active, so the reader keeps
# reading its snapshot and is never refused. Only a role change, which ends the
# era the snapshot recorded, refuses a read with WT_ROLLBACK.

import wiredtiger, wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_follower21(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    aux_uri = f'layered:{test_name}_aux'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)

    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
        return self.extensionsConfig() + self.conn_base_config + \
            'disaggregated=(role="follower")'

    def put(self, session, uri, kv, ts):
        # Commit a set of key/value pairs in a single transaction.
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for key, value in kv.items():
            cursor[key] = value
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def leader_checkpoint(self, ts):
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')
        self.session.checkpoint()

    def open_follower(self):
        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        session_follow.create(self.aux_uri, self.table_config)
        return conn_follow, session_follow

    def search(self, cursor, key):
        # Search that reports WT_ROLLBACK as an outcome rather than an error:
        # a correct implementation may refuse the read instead of serving it.
        cursor.set_key(key)
        try:
            ret = cursor.search()
        except wiredtiger.WiredTigerError as e:
            if 'WT_ROLLBACK' in str(e):
                return ('rollback', None)
            raise
        if ret == wiredtiger.WT_NOTFOUND:
            return ('notfound', None)
        self.assertEqual(ret, 0)
        return ('found', cursor.get_value())

    def setup_with_first_checkpoint(self):
        # Leader: baseline data sealed into a first checkpoint; follower
        # replicates the same writes into its ingest and picks it up.
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key_updated': 'old value'}, 10)
        self.put(self.session, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        self.put(session_follow, self.uri, {'key_updated': 'old value'}, 10)
        self.put(session_follow, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.disagg_advance_checkpoint(conn_follow)
        return conn_follow, session_follow

    def commit_post_snapshot_writes(self, conn_follow):
        # A writer commits an insert and an update in one transaction, on the
        # leader and (as replication into ingest) on the follower; the leader
        # seals it all into a new checkpoint and the follower picks it up.
        writes = {'key_inserted': 'new value', 'key_updated': 'new value 2'}
        self.put(self.session, self.uri, writes, 20)
        session_replay = conn_follow.open_session('')
        self.put(session_replay, self.uri, writes, 20)
        session_replay.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

    def check_new_data_visible(self, conn_follow):
        # Outside the racing transaction, the picked-up content must be there:
        # a fresh transaction sees the post-pickup writes. Callers get here with the
        # blocking snapshot ended, so the adoption it deferred can now be waited for.
        self.disagg_wait_for_adoption(conn_follow)
        session = conn_follow.open_session('')
        session.begin_transaction()
        cursor = session.open_cursor(self.uri)
        self.assertEqual(cursor['key_inserted'], 'new value')
        self.assertEqual(cursor['key_updated'], 'new value 2')
        cursor.close()
        session.rollback_transaction()
        session.close()

    def test_new_cursor_after_pickup(self):
        # Primary failure case: the transaction opens its first cursor on the
        # table after the pickup. The reader must not observe any part of the
        # post-snapshot writer transaction: the inserted key leaking from the
        # new checkpoint while the updated key still reads its old value would
        # expose half of the writer's transaction.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        # Establish the snapshot (no read timestamp) with a read on another table.
        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        state_inserted = self.search(cursor, 'key_inserted')
        self.assertEqual(state_inserted, ('notfound', None),
            'insert committed after the snapshot leaked through the picked-up checkpoint')
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'),
            'update committed after the snapshot leaked through the picked-up checkpoint')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()

        self.check_new_data_visible(conn_follow)
        conn_follow.close()

    def test_first_ever_pickup(self):
        # The transaction begins when the follower has no checkpoint at all,
        # so its reads come from ingest only. The first pickup then publishes
        # a checkpoint. Neither the cursor that was already reading (whose
        # view of the checkpoint is established on its next operation) nor a
        # brand-new cursor may observe the post-snapshot insert.
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key_base': 'base value'}, 10)

        conn_follow, session_follow = self.open_follower()
        self.put(session_follow, self.uri, {'key_base': 'base value'}, 10)

        session_follow.begin_transaction()
        cursor_before = session_follow.open_cursor(self.uri)
        cursor_before.set_key('key_base')
        self.assertEqual(cursor_before.search(), 0)
        self.assertEqual(cursor_before.get_value(), 'base value')

        # Post-snapshot writer, then the leader's first checkpoint.
        self.put(self.session, self.uri, {'key_inserted': 'new value'}, 20)
        session_replay = conn_follow.open_session('')
        self.put(session_replay, self.uri, {'key_inserted': 'new value'}, 20)
        session_replay.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        # The cursor that already returned pre-pickup results: a single cursor
        # must not mix results from before and after the pickup.
        state = self.search(cursor_before, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a cursor already reading before the pickup returned post-snapshot data')
        self.assertEqual(self.search(cursor_before, 'key_base'), ('found', 'base value'))

        # A brand-new cursor inside the same transaction.
        cursor_after = session_follow.open_cursor(self.uri)
        state = self.search(cursor_after, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a new cursor opened after the first-ever pickup returned post-snapshot data')
        cursor_after.close()
        session_follow.rollback_transaction()
        cursor_before.close()
        conn_follow.close()

    def test_nonrepeatable_read_within_transaction(self):
        # Two cursors in one transaction, one opened before the pickup and one
        # after, must agree: the same key must not be missing through one
        # cursor and present through the other.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        cursor_before = session_follow.open_cursor(self.uri)
        self.assertEqual(self.search(cursor_before, 'key_inserted'), ('notfound', None))

        self.commit_post_snapshot_writes(conn_follow)

        cursor_after = session_follow.open_cursor(self.uri)
        state = self.search(cursor_after, 'key_inserted')
        self.assertEqual(state, self.search(cursor_before, 'key_inserted'),
            'two cursors in one transaction disagree about a key across a pickup')
        self.assertEqual(state, ('notfound', None))
        cursor_after.close()
        session_follow.rollback_transaction()
        cursor_before.close()
        conn_follow.close()

    def test_scan_after_pickup(self):
        # Iteration and near-positioning through a cursor opened after the
        # pickup: a scan must return exactly the snapshot-visible keys. A scan
        # that surfaces the post-snapshot insert alongside the old value of
        # the updated key would expose half of the writer's transaction in a
        # single pass.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        contents = [tuple(kv) for kv in cursor]
        self.assertEqual(contents, [('key_updated', 'old value')],
            'a scan through a cursor opened after the pickup returned post-snapshot data')

        # Near-positioning must land on a snapshot-visible key.
        cursor.reset()
        cursor.set_key('key_a')
        self.assertEqual(cursor.search_near(), 1)
        self.assertEqual(cursor.get_key(), 'key_updated',
            'search_near positioned on a post-snapshot key')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_reverse_scan_after_pickup(self):
        # The reverse counterpart of the scan test: a backward walk and a
        # near-positioning from above must also return only snapshot-visible
        # keys after the pickup.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        contents = []
        while cursor.prev() == 0:
            contents.append((cursor.get_key(), cursor.get_value()))
        self.assertEqual(contents, [('key_updated', 'old value')],
            'a reverse scan through a cursor opened after the pickup returned post-snapshot '
            'data')

        # Near-positioning from above must land on the snapshot-visible key below.
        cursor.reset()
        cursor.set_key('key_z')
        self.assertEqual(cursor.search_near(), -1)
        self.assertEqual(cursor.get_key(), 'key_updated',
            'search_near positioned on a post-snapshot key')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_multiple_pickups(self):
        # Several pickups complete while the transaction is open. A new cursor
        # must not observe the writes sealed by any of them: consistency is
        # relative to the checkpoint in effect when the snapshot was
        # established, not merely the previous one.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        for key, ts in [('key_a', 20), ('key_b', 30)]:
            writes = {key: f'value {ts}'}
            self.put(self.session, self.uri, writes, ts)
            session_replay = conn_follow.open_session('')
            self.put(session_replay, self.uri, writes, ts)
            session_replay.close()
            self.leader_checkpoint(ts)
            self.disagg_advance_checkpoint(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        state = self.search(cursor, 'key_a')
        self.assertEqual(state, ('notfound', None),
            'a write sealed by an earlier mid-transaction pickup leaked')
        self.assertEqual(self.search(cursor, 'key_b'), ('notfound', None),
            'a write sealed by the latest mid-transaction pickup leaked')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_snapshot_visible_key_disappears(self):
        # The reverse anomaly: a key visible to the snapshot disappears. The
        # follower starts from the first checkpoint with an empty ingest
        # table, so the key exists only in checkpoint content. A writer then
        # removes it after the snapshot was established, and the removal is
        # sealed into the next checkpoint. A cursor opened after the pickup
        # must still see the key (or be refused), not miss it.
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key_removed': 'kept value'}, 10)
        self.put(self.session, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.leader_checkpoint(10)

        # The follower starts after the checkpoint: nothing is replayed into
        # its ingest table, all its reads come from checkpoint content.
        conn_follow, session_follow = self.open_follower()
        self.disagg_advance_checkpoint(conn_follow)

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        # The writer removes the key after the snapshot: on the leader, and
        # replayed into the follower's ingest.
        self.session.begin_transaction()
        leader_cursor = self.session.open_cursor(self.uri)
        leader_cursor.set_key('key_removed')
        self.assertEqual(leader_cursor.remove(), 0)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        leader_cursor.close()

        # The key only exists in checkpoint content, so the replayed remove
        # must position through it rather than blind-write into ingest.
        session_replay = conn_follow.open_session('')
        session_replay.begin_transaction()
        replay_cursor = session_replay.open_cursor(self.uri, None, 'overwrite=false')
        replay_cursor.set_key('key_removed')
        self.assertEqual(replay_cursor.remove(), 0)
        session_replay.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        replay_cursor.close()
        session_replay.close()

        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        state = self.search(cursor, 'key_removed')
        self.assertEqual(state, ('found', 'kept value'),
            'a key visible to the snapshot disappeared after the pickup')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def put_tables(self, session, kv_by_uri, ts):
        # Commit writes spanning several tables in a single transaction.
        session.begin_transaction()
        for uri, kv in kv_by_uri.items():
            cursor = session.open_cursor(uri)
            for key, value in kv.items():
                cursor[key] = value
            cursor.close()
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')

    def test_cross_table_torn_transaction(self):
        # A writer transaction spanning two tables. The reader holds a cursor
        # on the first table from before the pickup and opens a cursor on the
        # second table after it: the writer's transaction must not be visible
        # in one table and invisible in the other.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        cursor_a = session_follow.open_cursor(self.uri)
        self.assertEqual(self.search(cursor_a, 'key_updated'), ('found', 'old value'))

        writes = {self.uri: {'key_inserted': 'new value'},
                  self.aux_uri: {'anchor_inserted': 'new value'}}
        self.put_tables(self.session, writes, 20)
        session_replay = conn_follow.open_session('')
        self.put_tables(session_replay, writes, 20)
        session_replay.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        # The first table hides the writer through the pre-pickup cursor. The pickup marks the
        # constituents outdated, so even a pre-pickup cursor re-binds its stable constituent on
        # the next operation; without deferral that re-bind may be refused instead.
        state = self.search(cursor_a, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a multi-table writer transaction leaked into a snapshot after a pickup')

        # Opening the second table's cursor after the pickup may itself be refused.
        try:
            cursor_b = session_follow.open_cursor(self.aux_uri)
        except wiredtiger.WiredTigerError as e:
            self.assertTrue(wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e))
            cursor_b = None
        if cursor_b is not None:
            state = self.search(cursor_b, 'anchor_inserted')
            self.assertEqual(state, ('notfound', None),
                'a multi-table writer transaction is torn across tables')
            cursor_b.close()
        session_follow.rollback_transaction()
        cursor_a.close()
        conn_follow.close()
        self.ignoreStderrPatternIfExists('(WT_ROLLBACK|WT_NOTFOUND)')

    def test_cursor_cache_reopen_after_pickup(self):
        # A cursor closed before the pickup and reopened afterwards (likely
        # through the session cursor cache) is as new as a first open: the
        # reopened cursor must not observe the post-snapshot writes either.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        cursor = session_follow.open_cursor(self.uri)
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))
        cursor.close()

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        state = self.search(cursor, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a cursor reopened after the pickup returned post-snapshot data')
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))
        session_follow.rollback_transaction()
        cursor.close()
        conn_follow.close()

    def test_table_created_after_snapshot(self):
        # A table created after the snapshot, with its content sealed into the
        # picked-up checkpoint. A cursor on it inside the old transaction must
        # not return any rows: all of them were committed after the snapshot.
        conn_follow, session_follow = self.setup_with_first_checkpoint()
        late_uri = f'layered:{self.test_name}_late'

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.session.create(late_uri, self.table_config)
        self.put(self.session, late_uri, {'key_late': 'late value'}, 20)
        session_replay = conn_follow.open_session('')
        session_replay.create(late_uri, self.table_config)
        self.put(session_replay, late_uri, {'key_late': 'late value'}, 20)
        session_replay.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        cursor = session_follow.open_cursor(late_uri)
        contents = [tuple(kv) for kv in cursor]
        self.assertEqual(contents, [],
            'rows of a table created after the snapshot are visible')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_truncated_range_disappears(self):
        # Fast-truncate variant of the disappearing-key anomaly. The follower
        # starts from the first checkpoint with an empty ingest table; a range
        # visible to the snapshot is truncated after it and the truncation is
        # sealed into the next checkpoint. A cursor opened after the pickup
        # must still see the whole range (or be refused).
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        keys = {f'key_r{i}': f'value {i}' for i in range(5)}
        self.put(self.session, self.uri, keys, 10)
        self.put(self.session, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        self.disagg_advance_checkpoint(conn_follow)

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        # Truncate a sub-range after the snapshot: on the leader, and replayed
        # into the follower's ingest.
        def truncate_range(session):
            session.begin_transaction()
            lo = session.open_cursor(self.uri)
            hi = session.open_cursor(self.uri)
            lo.set_key('key_r1')
            hi.set_key('key_r3')
            session.truncate(None, lo, hi, None)
            session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
            lo.close()
            hi.close()
        truncate_range(self.session)
        session_replay = conn_follow.open_session('')
        truncate_range(session_replay)
        session_replay.close()
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        contents = [key for key, _ in cursor]
        self.assertEqual(contents, sorted(keys.keys()),
            'a range visible to the snapshot disappeared after the pickup')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_random_cursor_after_pickup(self):
        # Random sampling through a cursor opened after the pickup. The table
        # holds nothing visible to the snapshot, so sampling must come up
        # empty rather than return a post-snapshot key.
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        self.put(session_follow, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.disagg_advance_checkpoint(conn_follow)

        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri, None, 'next_random=true')
        ret = cursor.next()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND,
            'random sampling returned a key committed after the snapshot')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_timestamped_reader_oldest_advanced(self):
        # Boundary of the timestamped-reader guarantee: the picked-up
        # checkpoint's oldest timestamp has moved past the reader's read
        # timestamp, so the history the reader needs may be gone from the new
        # checkpoint. The reader must get its consistent old value or a
        # rollback, never the newer value or a missing key.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        writes = {'key_updated': 'new value 2'}
        self.put(self.session, self.uri, writes, 20)
        session_replay = conn_follow.open_session('')
        self.put(session_replay, self.uri, writes, 20)
        session_replay.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(20)}')
        self.session.checkpoint()
        try:
            self.disagg_advance_checkpoint(conn_follow)
        except wiredtiger.WiredTigerError as e:
            # Refusing to adopt a checkpoint whose oldest timestamp is ahead
            # of an active reader is acceptable: the reader keeps its view.
            self.assertTrue('Invalid argument' in str(e))
            session_follow.rollback_transaction()
            aux_cursor.close()
            conn_follow.close()
            return

        cursor = session_follow.open_cursor(self.uri)
        cursor.set_key('key_updated')
        try:
            ret = cursor.search()
            if ret == 0:
                self.assertEqual(cursor.get_value(), 'old value',
                    'a timestamped reader lost its view when oldest moved past its read timestamp')
            else:
                self.fail('a key visible at the read timestamp went missing')
        except wiredtiger.WiredTigerError as e:
            # Refusing the read outright is acceptable: reading below the new
            # oldest timestamp must fail rather than return a wrong result.
            self.assertTrue('WT_ROLLBACK' in str(e) or 'Invalid argument' in str(e))
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_inherited_cursor_across_transactions(self):
        # A cursor kept open across transactions. Its stable view was
        # established under the previous transaction's snapshot, so its first
        # use in a new transaction may advance to the newest checkpoint - but
        # not to one adopted after the new transaction's snapshot.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        cursor = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction()
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))
        session_follow.commit_transaction()

        # A write commit between the transactions, so the inherited cursor
        # sees a changed snapshot and becomes eligible to advance.
        writes = {'key_between': 'between value'}
        self.put(self.session, self.uri, writes, 15)
        session_replay = conn_follow.open_session('')
        self.put(session_replay, self.uri, writes, 15)
        session_replay.close()

        # The next transaction establishes its snapshot before the pickup.
        session_follow.begin_transaction()
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        state = self.search(cursor, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a cursor inherited from an earlier transaction advanced past the snapshot')
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_step_down_mid_transaction(self):
        # The leader steps down while a transaction at snapshot isolation
        # without a read timestamp is open. The stepped-down node serves reads
        # from its own last checkpoint, whose content is treated like any
        # adopted checkpoint: a post-snapshot insert sealed into it must not
        # become visible to the old transaction.
        self.session.create(self.uri, self.table_config)
        self.session.create(self.aux_uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key_updated': 'old value'}, 10)
        self.put(self.session, self.aux_uri, {'anchor': 'anchor value'}, 10)
        self.leader_checkpoint(10)

        session_txn = self.conn.open_session('')
        session_txn.begin_transaction()
        aux_cursor = session_txn.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        # A post-snapshot write sealed into the last checkpoint, then step down.
        self.put(self.session, self.uri, {'key_inserted': 'new value'}, 20)
        self.leader_checkpoint(20)
        self.conn.reconfigure('disaggregated=(role="follower")')

        # The role era the snapshot recorded ended, so the bind is refused; what
        # it must never do is serve the write committed after the snapshot.
        cursor = session_txn.open_cursor(self.uri)
        self.assertEqual(self.search(cursor, 'key_inserted'), ('rollback', None),
            'a post-snapshot write became visible across a step-down')
        session_txn.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        session_txn.close()

    def test_timestamped_reader_unaffected(self):
        # A reader with a read timestamp must keep its consistent view across
        # the pickup with no rollbacks: the history preserved with the
        # checkpoint provides the timestamped view.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        aux_cursor = session_follow.open_cursor(self.aux_uri)
        aux_cursor.set_key('anchor')
        self.assertEqual(aux_cursor.search(), 0)

        self.commit_post_snapshot_writes(conn_follow)

        cursor = session_follow.open_cursor(self.uri)
        cursor.set_key('key_inserted')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.set_key('key_updated')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'old value')
        session_follow.rollback_transaction()
        cursor.close()
        aux_cursor.close()
        conn_follow.close()

    def test_open_cursor_on_table_unaffected(self):
        # A cursor that was already reading the table before the pickup must
        # never see the post-snapshot writes. The cursor's stable constituent
        # opens lazily, so the first read needing it may fall after the pickup
        # and be refused instead of served; a refusal is an acceptable outcome.
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        session_follow.begin_transaction()
        cursor = session_follow.open_cursor(self.uri)
        cursor.set_key('key_updated')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'old value')

        self.commit_post_snapshot_writes(conn_follow)

        state = self.search(cursor, 'key_inserted')
        self.assertEqual(state, ('notfound', None),
            'a post-snapshot write leaked into an open cursor after a pickup')
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))
        session_follow.rollback_transaction()
        cursor.close()
        conn_follow.close()
        self.ignoreStderrPatternIfExists('(WT_ROLLBACK|WT_NOTFOUND)')

    def test_transaction_after_pickup_unaffected(self):
        # A snapshot transaction that begins after the pickup must see the
        # picked-up content with no rollbacks.
        conn_follow, session_follow = self.setup_with_first_checkpoint()
        self.commit_post_snapshot_writes(conn_follow)
        self.disagg_wait_for_adoption(conn_follow)

        session_follow.begin_transaction()
        cursor = session_follow.open_cursor(self.uri)
        self.assertEqual(self.search(cursor, 'key_inserted'), ('found', 'new value'))
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'new value 2'))
        session_follow.rollback_transaction()
        cursor.close()
        conn_follow.close()

    def test_step_down_restarts_deferred_pickup(self):
        # A node that steps up stops the deferred pickup server; stepping back
        # down must start it again, or a checkpoint deferred afterwards would
        # never be adopted. The deferral is what makes this observable: only the
        # server can adopt a checkpoint after the transaction blocking it ends.
        self.ignoreStdoutPattern('Picking up the same checkpoint again')
        conn_follow, session_follow = self.setup_with_first_checkpoint()

        # Round trip through the leader role and back.
        self.disagg_switch_follower_and_leader(conn_follow, self.conn)
        self.disagg_switch_follower_and_leader(self.conn, conn_follow)

        # A transaction holds a snapshot while the next checkpoint is delivered,
        # so its adoption is deferred; nothing but the pickup server will do it.
        session_hold = conn_follow.open_session('')
        session_hold.begin_transaction()
        cursor = session_hold.open_cursor(self.uri)
        self.assertEqual(self.search(cursor, 'key_updated'), ('found', 'old value'))

        self.put(self.session, self.uri, {'key_final': 'final value'}, 30)
        self.leader_checkpoint(30)
        self.disagg_advance_checkpoint(conn_follow)

        # Ending the transaction releases the pin: the restarted server adopts.
        cursor.close()
        session_hold.rollback_transaction()
        session_hold.close()
        # With the snapshot gone, nothing blocks the adoption. It still may not run inline: any
        # snapshot the node happens to hold, including the ones the adoption itself takes, defers
        # the delivery to the server, so wait for it before reading.
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        self.disagg_wait_for_adoption(conn_follow)

        session = conn_follow.open_session('')
        session.begin_transaction()
        cursor = session.open_cursor(self.uri)
        self.assertEqual(self.search(cursor, 'key_final'), ('found', 'final value'))
        session.rollback_transaction()
        cursor.close()
        session.close()
        conn_follow.close()
