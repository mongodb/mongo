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
from helper import simulate_crash_restart
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Expect each warning exactly once in the output and nothing else.
def check_drain_warnings(test, expected):
    def validate(output):
        lines = [line for line in output.splitlines() if line.strip()]
        for line in lines:
            test.assertTrue(any(warning in line for warning in expected),
                'unexpected output: ' + line)
        for warning in expected:
            test.assertEqual(sum(warning in line for line in lines), 1, warning)
    return validate

# Blind removes through the layered cursor survive step-up, checkpoint and restart, including
# prepared transactions resolved on the new leader.
@disagg_test_class
class test_layered_stepup17(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup17'
    # Removes of keys that always have a value in the stable table.
    live_uri = uri + '_live'
    # Removes of keys that never exist, with a count different from the main table.
    orphan_uri = uri + '_orphan'
    # None means a read without a read timestamp.
    read_ts = (6, 8, 9, 15, 20, 22, 23, 24, 25, 26, 27, 28, None)

    stable_states = [
        ('absent', dict(stable_state='absent')),
        ('deleted', dict(stable_state='deleted')),
        ('present', dict(stable_state='present')),
    ]
    workloads = [
        ('committed', dict(prepared=False, commit=True, repeat_remove=False,
            insert_after_remove=False)),
        ('repeated', dict(prepared=False, commit=True, repeat_remove=True,
            insert_after_remove=False)),
        ('reinserted', dict(prepared=False, commit=True, repeat_remove=False,
            insert_after_remove=True)),
        ('prepared_commit', dict(prepared=True, commit=True, repeat_remove=False,
            insert_after_remove=False)),
        ('prepared_rollback', dict(prepared=True, commit=False, repeat_remove=False,
            insert_after_remove=False)),
        ('prepared_reinserted', dict(prepared=True, commit=True, repeat_remove=False,
            insert_after_remove=True)),
    ]
    key_counts = [
        ('one_key', dict(nkeys=1, orphan_tables=1)),
        ('many_keys', dict(nkeys=3, orphan_tables=1)),
        ('many_tables', dict(nkeys=1, orphan_tables=15)),
    ]
    scenarios = make_scenarios(
        gen_disagg_storages(disagg_only=True), stable_states, workloads, key_counts,
        include=lambda name, s: s['orphan_tables'] == 1 or (
            s['prepared'] and not s['insert_after_remove'] and s['stable_state'] != 'present'))

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),precise_checkpoint=true,' \
            'preserve_prepared=true,disaggregated=(role="leader")'

    def remove_keys(self, session, uri, keys):
        cursor = session.open_cursor(uri, None, 'blind_remove=true')
        for key in keys:
            cursor.set_key(key)
            self.assertEqual(cursor.remove(), 0)
        cursor.close()

    def check_value(self, cursor, key, expected):
        cursor.set_key(key)
        ret = cursor.search()
        if expected is None:
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(ret, 0)
            self.assertEqual(cursor.get_value(), expected)

    def check_reads(self):
        remove_ts = 25 if self.prepared else 20
        for read_ts in self.read_ts:
            ts = 100 if read_ts is None else read_ts
            with self.transaction(read_timestamp=read_ts, rollback=True):
                cursor = self.session.open_cursor(self.uri)
                expected = b'old' if self.stable_state == 'present' or (
                    self.stable_state == 'deleted' and ts < 8) else None
                if self.commit and ts >= remove_ts:
                    expected = None
                if self.insert_after_remove and ts >= self.reinsert_ts:
                    expected = b'new'
                for key in self.keys:
                    self.check_value(cursor, key, expected)
                self.check_value(cursor, 'anchor', b'anchor')
                self.check_value(cursor, 'kept',
                    b'kept' if self.commit and ts >= remove_ts else None)
                cursor.close()

                cursor = self.session.open_cursor(self.live_uri)
                self.check_value(cursor, '1',
                    None if self.commit and ts >= remove_ts else b'live')
                cursor.close()

                for uri in self.orphan_uris:
                    cursor = self.session.open_cursor(uri)
                    for key in self.orphan_keys:
                        self.check_value(cursor, key, None)
                    self.check_value(cursor, 'anchor', b'anchor')
                    cursor.close()

    def test_blind_remove_then_step_up(self):
        self.keys = [str(i) for i in range(1, self.nkeys + 1)]
        self.orphan_keys = [str(i) for i in range(1, self.nkeys + 2)]
        self.orphan_uris = [f'{self.orphan_uri}_{i}' for i in range(self.orphan_tables)]
        self.reinsert_ts = 27 if self.prepared else 25
        for uri in [self.uri, self.live_uri] + self.orphan_uris:
            self.session.create(uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=5):
            cursor['anchor'] = b'anchor'
            if self.stable_state != 'absent':
                for key in self.keys:
                    cursor[key] = b'old'
        cursor.close()
        if self.stable_state == 'deleted':
            with self.transaction(commit_timestamp=8):
                self.remove_keys(self.session, self.uri, self.keys)
        with self.transaction(commit_timestamp=5):
            cursor = self.session.open_cursor(self.live_uri)
            cursor['1'] = b'live'
            cursor.close()
            for uri in self.orphan_uris:
                cursor = self.session.open_cursor(uri)
                cursor['anchor'] = b'anchor'
                cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')
        writer = self.conn.open_session()
        opens = self.get_stat(wiredtiger.stat.conn.layered_curs_open_stable)
        removes = self.get_stat(wiredtiger.stat.conn.layered_curs_blind_remove_follower)
        already_deleted = self.get_stat(
            wiredtiger.stat.conn.layered_curs_blind_remove_ingest_deleted)

        writer.begin_transaction()
        self.remove_keys(writer, self.uri, self.keys)
        self.remove_keys(writer, self.live_uri, ['1'])
        for uri in self.orphan_uris:
            self.remove_keys(writer, uri, self.orphan_keys)
        cursor = writer.open_cursor(self.uri)
        cursor['kept'] = b'kept'
        cursor.close()
        if self.prepared:
            writer.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20) +
                ',prepared_id=' + self.prepared_id_str(1))
        else:
            writer.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
            if self.repeat_remove:
                # A second remove at a later timestamp succeeds without adding another delete.
                with self.transaction(session=writer, commit_timestamp=23):
                    self.remove_keys(writer, self.uri, self.keys)

        self.assertEqual(self.get_stat(wiredtiger.stat.conn.layered_curs_open_stable), opens)
        self.assertEqual(
            self.get_stat(wiredtiger.stat.conn.layered_curs_blind_remove_follower) - removes,
            self.nkeys * (2 if self.repeat_remove else 1) + 1 +
            len(self.orphan_keys) * self.orphan_tables)
        self.assertEqual(self.get_stat(
            wiredtiger.stat.conn.layered_curs_blind_remove_ingest_deleted) - already_deleted,
            self.nkeys if self.repeat_remove else 0)

        if self.insert_after_remove and not self.prepared:
            cursor = writer.open_cursor(self.uri)
            with self.transaction(session=writer, commit_timestamp=self.reinsert_ts):
                for key in self.keys:
                    cursor[key] = b'new'
            cursor.close()
        if not self.prepared:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        orphans = {uri: len(self.orphan_keys) for uri in self.orphan_uris}
        if self.stable_state != 'present':
            orphans[self.uri] = self.nkeys
        warnings = [f'{uri}: step-up drain skipped orphan removes for {count} key(s) with no '
            'value in the stable table' for uri, count in orphans.items()]
        before = self.get_stat(wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove)
        with self.customStdoutPattern(check_drain_warnings(self, warnings)):
            self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.get_stat(
            wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove) - before,
            sum(orphans.values()))

        if self.prepared:
            if self.commit:
                writer.commit_transaction('commit_timestamp=' + self.timestamp_str(25) +
                    ',durable_timestamp=' + self.timestamp_str(26))
            else:
                writer.rollback_transaction('rollback_timestamp=' + self.timestamp_str(25))
            if self.insert_after_remove:
                cursor = writer.open_cursor(self.uri)
                with self.transaction(session=writer, commit_timestamp=self.reinsert_ts):
                    for key in self.keys:
                        cursor[key] = b'new'
                cursor.close()
        writer.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.check_reads()
        self.session.checkpoint()
        self.check_reads()
        self.reopen_conn()
        self.check_reads()

# Prepared operations on a missing key are rolled back on the follower before a blind remove of the
# key. Rolled-back operations are invisible, so only the blind remove can be an orphan.
@disagg_test_class
class test_layered_stepup17_rollback(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup17_rollback'
    read_ts = (11, 12, 13, 15, 16, 20, 25, 40, None)

    rolled_back = [
        ('remove', dict(rolled_back=['remove'])),
        ('remove_remove', dict(rolled_back=['remove', 'remove'])),
        ('insert', dict(rolled_back=['insert'])),
        ('insert_remove', dict(rolled_back=['insert', 'remove'])),
        ('remove_insert', dict(rolled_back=['remove', 'insert'])),
    ]
    finals = [
        ('no_remove', dict(final=None, insert_in_txn=False, reinsert=False)),
        ('committed', dict(final='commit', insert_in_txn=False, reinsert=False)),
        ('committed_reinsert', dict(final='commit', insert_in_txn=False, reinsert=True)),
        ('prepared_commit', dict(final='prepared_commit', insert_in_txn=False, reinsert=False)),
        ('prepared_rollback', dict(final='prepared_rollback', insert_in_txn=False, reinsert=False)),
        ('prepared_insert_commit', dict(final='prepared_commit', insert_in_txn=True,
            reinsert=False)),
        ('prepared_insert_rollback', dict(final='prepared_rollback', insert_in_txn=True,
            reinsert=False)),
    ]
    checkpoint_timestamps = [
        ('rollback_pending', dict(checkpoint_ts=11)),
        ('second_rollback_pending', dict(checkpoint_ts=13)),
        ('rollback_stable', dict(checkpoint_ts=19)),
    ]
    scenarios = make_scenarios(
        gen_disagg_storages(disagg_only=True), rolled_back, finals, checkpoint_timestamps)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),precise_checkpoint=true,' \
            'preserve_prepared=true,disaggregated=(role="leader")'

    def blind_remove(self, session):
        cursor = session.open_cursor(self.uri, None, 'blind_remove=true')
        cursor.set_key('k')
        self.assertEqual(cursor.remove(), 0)
        cursor.close()

    def check_reads(self):
        for read_ts in self.read_ts:
            with self.transaction(read_timestamp=read_ts, rollback=True):
                cursor = self.session.open_cursor(self.uri)
                cursor.set_key('k')
                ts = 100 if read_ts is None else read_ts
                if (self.insert_in_txn and self.final == 'prepared_commit' and ts >= 25) or (
                    self.reinserted and ts >= 22):
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), b'new')
                else:
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                self.assertEqual(cursor['anchor'], b'anchor')
                cursor.close()

    def test_rollback_then_blind_remove(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        with self.transaction(commit_timestamp=5):
            cursor = self.session.open_cursor(self.uri)
            cursor['anchor'] = b'anchor'
            cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')
        self.reinserted = self.reinsert
        writer = self.conn.open_session()
        for i, op in enumerate(self.rolled_back):
            ts = 11 + 2 * i
            writer.begin_transaction()
            if op == 'remove':
                self.blind_remove(writer)
            else:
                cursor = writer.open_cursor(self.uri)
                cursor['k'] = b'rolled back'
                cursor.close()
            writer.prepare_transaction('prepare_timestamp=' + self.timestamp_str(ts) +
                ',prepared_id=' + self.prepared_id_str(i + 1))
            writer.rollback_transaction('rollback_timestamp=' + self.timestamp_str(ts + 1))
        if self.final is not None:
            writer.begin_transaction()
            self.blind_remove(writer)
            if self.insert_in_txn:
                cursor = writer.open_cursor(self.uri)
                cursor['k'] = b'new'
                cursor.close()
            if self.final == 'commit':
                writer.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
            else:
                writer.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20) +
                    ',prepared_id=' + self.prepared_id_str(len(self.rolled_back) + 1))
        if self.reinsert:
            writer.begin_transaction()
            cursor = writer.open_cursor(self.uri)
            cursor['k'] = b'new'
            cursor.close()
            writer.commit_transaction('commit_timestamp=' + self.timestamp_str(22))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.checkpoint_ts))
        orphans = 0 if self.final is None else 1
        warnings = [f'{self.uri}: step-up drain skipped orphan removes for 1 key(s) with no value '
            'in the stable table'] if orphans else []
        before = self.get_stat(wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove)
        with self.customStdoutPattern(check_drain_warnings(self, warnings)):
            self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.get_stat(
            wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove) - before, orphans)

        if self.final == 'prepared_commit':
            writer.commit_transaction('commit_timestamp=' + self.timestamp_str(25) +
                ',durable_timestamp=' + self.timestamp_str(25))
        elif self.final == 'prepared_rollback':
            writer.rollback_transaction('rollback_timestamp=' + self.timestamp_str(25))
        writer.close()

        # Verify reads across checkpoints and recovery at each stable timestamp.
        self.check_reads()
        self.session.checkpoint()
        self.check_reads()
        restart_dir = '.'
        if self.checkpoint_ts != 19 and self.final in (None, 'commit'):
            restart_dir = 'RESTART'
            simulate_crash_restart(self, '.', restart_dir)
            # The reinsert is newer than the last checkpoint.
            self.reinserted = False
            discover = self.session.open_cursor('prepared_discover:')
            recovered = []
            while discover.next() == 0:
                recovered.append(discover.get_key())
            expected = [i + 1 for i, op in enumerate(self.rolled_back) if op == 'insert' and
                11 + 2 * i <= self.checkpoint_ts < 12 + 2 * i]
            self.assertEqual(sorted(recovered), expected)
            for prepared_id in recovered:
                resolver = self.conn.open_session()
                resolver.begin_transaction(
                    'claim_prepared_id=' + self.prepared_id_str(prepared_id))
                resolver.rollback_transaction('rollback_timestamp=' + self.timestamp_str(
                    12 + 2 * (prepared_id - 1)))
                resolver.close()
            discover.close()
            self.check_reads()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.session.checkpoint()
        self.check_reads()
        self.reopen_conn(restart_dir)
        self.check_reads()

# A key goes through remove, insert and remove before step-up, or the oldest timestamp moves past
# the remove. The step-up drain cannot treat a follower remove as globally visible before the last
# checkpoint includes it, so a remove with nothing to delete is still skipped.
@disagg_test_class
class test_layered_stepup17_stacked(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup17_stacked'

    stable_states = [
        ('absent', dict(stable_state='absent')),
        ('deleted', dict(stable_state='deleted')),
        ('present', dict(stable_state='present')),
    ]
    workloads = [
        ('remove', dict(reinsert=False)),
        ('remove_insert_remove', dict(reinsert=True)),
    ]
    oldest = [
        ('oldest_pinned', dict(oldest_ts=1)),
        ('oldest_past_remove', dict(oldest_ts=30)),
    ]
    scenarios = make_scenarios(
        gen_disagg_storages(disagg_only=True), stable_states, workloads, oldest)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),precise_checkpoint=true,' \
            'preserve_prepared=true,disaggregated=(role="leader")'

    def blind_remove(self, ts):
        with self.transaction(commit_timestamp=ts):
            cursor = self.session.open_cursor(self.uri, None, 'blind_remove=true')
            cursor.set_key('k')
            self.assertEqual(cursor.remove(), 0)
            cursor.close()

    def expected(self, ts):
        if ts >= 20:
            return b'new' if self.reinsert and 22 <= ts < 24 else None
        if self.stable_state == 'present' or (self.stable_state == 'deleted' and ts < 8):
            return b'old'
        return None

    def check_reads(self):
        read_ts = (6, 9, 15, 20, 21, 22, 23, 24, 25, 30, None) if self.oldest_ts == 1 else \
            (30, 40, None)
        for ts in read_ts:
            with self.transaction(read_timestamp=ts, rollback=True):
                cursor = self.session.open_cursor(self.uri)
                cursor.set_key('k')
                value = self.expected(100 if ts is None else ts)
                if value is None:
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                else:
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), value)
                self.assertEqual(cursor['anchor'], b'anchor')
                cursor.close()

    def test_stacked_removes_then_step_up(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        with self.transaction(commit_timestamp=5):
            cursor = self.session.open_cursor(self.uri)
            cursor['anchor'] = b'anchor'
            if self.stable_state != 'absent':
                cursor['k'] = b'old'
            cursor.close()
        if self.stable_state == 'deleted':
            with self.transaction(commit_timestamp=8):
                cursor = self.session.open_cursor(self.uri)
                cursor.set_key('k')
                self.assertEqual(cursor.remove(), 0)
                cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')
        self.blind_remove(20)
        if self.reinsert:
            with self.transaction(commit_timestamp=22):
                cursor = self.session.open_cursor(self.uri)
                cursor['k'] = b'new'
                cursor.close()
            self.blind_remove(24)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        if self.oldest_ts != 1:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.oldest_ts))
        orphans = 0 if self.stable_state == 'present' else 1
        warnings = [f'{self.uri}: step-up drain skipped orphan removes for 1 key(s) with no value '
            'in the stable table'] if orphans else []
        before = self.get_stat(wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove)
        with self.customStdoutPattern(check_drain_warnings(self, warnings)):
            self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.get_stat(
            wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove) - before, orphans)

        self.check_reads()
        self.session.checkpoint()
        self.check_reads()
        self.reopen_conn()
        self.check_reads()

# A prepared transaction writes a key several times on the follower and is resolved before or
# after step-up. A first write that removes a key with no value in the stable table is an orphan.
@disagg_test_class
class test_layered_stepup17_same_txn(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup17_same_txn'
    read_ts = (15, 22, 25, 30, None)

    writes = [
        ('remove_insert', dict(writes=['remove', 'insert'])),
        ('insert_remove', dict(writes=['insert', 'remove'])),
        ('remove_insert_remove', dict(writes=['remove', 'insert', 'remove'])),
        ('insert_update', dict(writes=['insert', 'update'])),
    ]
    stable_states = [
        ('absent', dict(stable_state='absent')),
        ('present', dict(stable_state='present')),
    ]
    resolutions = [
        ('commit_after', dict(commit=True, resolve_before=False)),
        ('rollback_after', dict(commit=False, resolve_before=False)),
        ('commit_before', dict(commit=True, resolve_before=True)),
        ('rollback_before', dict(commit=False, resolve_before=True)),
    ]
    scenarios = make_scenarios(
        gen_disagg_storages(disagg_only=True), writes, stable_states, resolutions)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),precise_checkpoint=true,' \
            'preserve_prepared=true,disaggregated=(role="leader")'

    def resolve(self, session):
        if self.commit:
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(25) +
                ',durable_timestamp=' + self.timestamp_str(25))
        else:
            session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(25))

    def expected(self, ts):
        if self.commit and ts >= 25:
            last = self.writes[-1]
            return None if last == 'remove' else last.encode()
        return b'old' if self.stable_state == 'present' else None

    def check_reads(self):
        for read_ts in self.read_ts:
            with self.transaction(read_timestamp=read_ts, rollback=True):
                cursor = self.session.open_cursor(self.uri)
                cursor.set_key('k')
                value = self.expected(100 if read_ts is None else read_ts)
                if value is None:
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                else:
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), value)
                self.assertEqual(cursor['anchor'], b'anchor')
                cursor.close()

    def test_same_txn_writes_then_step_up(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        with self.transaction(commit_timestamp=5):
            cursor = self.session.open_cursor(self.uri)
            cursor['anchor'] = b'anchor'
            if self.stable_state == 'present':
                cursor['k'] = b'old'
            cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')
        writer = self.conn.open_session()
        writer.begin_transaction()
        for write in self.writes:
            if write == 'remove':
                cursor = writer.open_cursor(self.uri, None, 'blind_remove=true')
                cursor.set_key('k')
                self.assertEqual(cursor.remove(), 0)
            else:
                cursor = writer.open_cursor(self.uri)
                cursor['k'] = write.encode()
            cursor.close()
        writer.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20) +
            ',prepared_id=' + self.prepared_id_str(1))
        if self.resolve_before:
            self.resolve(writer)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(22))

        # Removes rolled back before step-up are not counted.
        orphans = 1 if self.stable_state == 'absent' and self.writes[0] == 'remove' and not (
            self.resolve_before and not self.commit) else 0
        warnings = [f'{self.uri}: step-up drain skipped orphan removes for 1 key(s) with no value '
            'in the stable table'] if orphans else []
        before = self.get_stat(wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove)
        with self.customStdoutPattern(check_drain_warnings(self, warnings)):
            self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.get_stat(
            wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove) - before, orphans)

        # Checkpoint while the transaction may still be prepared, then resolve it.
        self.session.checkpoint()
        if not self.resolve_before:
            self.resolve(writer)
        writer.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.check_reads()
        self.session.checkpoint()
        self.check_reads()
        self.reopen_conn()
        self.check_reads()
