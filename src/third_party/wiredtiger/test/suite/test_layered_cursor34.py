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
from wiredtiger import stat
from wtscenario import make_scenarios

# Cursor position, bounds, conflicts, checkpoints, step-up and configuration for a follower remove
# through a blind_remove cursor.
@disagg_test_class
class test_layered_cursor34(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = 'layered:' + test_name
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    stable_items = {'anchor': 'anchor', 'stable_key': 'stable_val', 'z_stable': 'z_val'}

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, self.table_config)
        cursor = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=10):
            for key, value in self.stable_items.items():
                cursor[key] = value
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, self.table_config)
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

        cursor = self.session_follow.open_cursor(self.uri)
        with self.transaction(session=self.session_follow, commit_timestamp=20):
            cursor['ingest_key'] = 'ingest_val'
        cursor.close()

    def get_follower_stat(self, stat_id):
        return self.get_stat(stat_id, session=self.session_follow)

    def search(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        ret = cursor.search()
        value = cursor.get_value() if ret == 0 else None
        cursor.close()
        return ret, value

    # Removes on a positioned cursor keep its position, including a remove of a key the cursor has
    # already removed.
    def test_positioned_remove(self):
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
        self.session_follow.begin_transaction()
        cursor.set_key('anchor')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.remove(), 0)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'ingest_key')
        self.assertEqual(cursor.remove(), 0)

        removed = self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted)
        self.assertEqual(cursor.remove(), 0)
        self.assertEqual(
            self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted), removed + 1)

        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'stable_key')
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        for key in ('anchor', 'ingest_key'):
            self.assertEqual(self.search(self.session_follow, key)[0], wiredtiger.WT_NOTFOUND)
        self.assertEqual(self.search(self.session_follow, 'stable_key'), (0, 'stable_val'))

    def ingest_versions(self, key):
        cursor = self.session_follow.open_cursor('file:' + self.test_name + '.wt_ingest', None,
            'debug=(dump_version=(enabled=true))')
        cursor.set_key(key)
        count = 0
        if cursor.search() == 0:
            count = 1
            while cursor.next() == 0 and cursor.get_key() == key:
                count += 1
        cursor.close()
        return count

    # Removing a key twice during a walk, forward or backward, writes only one tombstone, and the
    # walk can carry on from the removed key.
    def test_walk_double_remove(self):
        for forward, removed_key, next_key in ((True, 'ingest_key', 'stable_key'),
                (False, 'stable_key', 'ingest_key')):
            cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
            step = cursor.next if forward else cursor.prev
            self.session_follow.begin_transaction()
            while step() == 0 and cursor.get_key() != removed_key:
                pass
            self.assertEqual(cursor.get_key(), removed_key)
            self.assertEqual(cursor.remove(), 0)
            versions = self.ingest_versions(removed_key)

            removed = self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted)
            self.assertEqual(cursor.remove(), 0)
            self.assertEqual(
                self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted),
                removed + 1)
            self.assertEqual(self.ingest_versions(removed_key), versions)

            self.assertEqual(cursor.get_key(), removed_key)
            self.assertEqual(step(), 0)
            self.assertEqual(cursor.get_key(), next_key)
            self.session_follow.rollback_transaction()
            cursor.close()

    # Commits a remove of a key from another session.
    def remove_key(self, key):
        session = self.conn_follow.open_session('')
        cursor = session.open_cursor(self.uri)
        with self.transaction(session=session, commit_timestamp=25):
            cursor.set_key(key)
            self.assertEqual(cursor.remove(), 0)
        session.close()

    # Commits a truncate of a single key from another session.
    def truncate_key(self, key):
        session = self.conn_follow.open_session('')
        start = session.open_cursor(self.uri)
        stop = session.open_cursor(self.uri)
        start.set_key(key)
        stop.set_key(key)
        with self.transaction(session=session, commit_timestamp=25):
            session.truncate(None, start, stop, None)
        session.close()

    # Cursors positioned on a key in an earlier transaction, by search or by a walk in either
    # direction, remove the key after it was removed underneath them. Nothing is written. A cursor
    # that keeps its position carries on its walk from the removed key, otherwise the walk starts
    # over.
    def check_remove_removed_key(self, key, value, remove_underneath, read_uncommitted=False,
            keeps_position=True):
        keys = ['anchor', 'ingest_key', 'stable_key', 'z_stable']
        i = keys.index(key)
        cursors = []
        if read_uncommitted:
            self.session_follow.begin_transaction('isolation=read-uncommitted')
        for by_search in (True, False):
            for forward in (True, False):
                cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
                step = cursor.next if forward else cursor.prev
                if by_search:
                    cursor.set_key(key)
                    self.assertEqual(cursor.search(), 0)
                else:
                    while step() == 0 and cursor.get_key() != key:
                        pass
                    self.assertEqual(cursor.get_key(), key)
                self.assertEqual(cursor.get_value(), value)
                if keeps_position:
                    rest = keys[i + 1:] if forward else list(reversed(keys[:i]))
                else:
                    rest = [k for k in (keys if forward else reversed(keys)) if k != key]
                cursors.append((cursor, step, rest))
        if read_uncommitted:
            self.session_follow.commit_transaction()
        remove_underneath()

        removed = self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted)
        self.session_follow.begin_transaction()
        for cursor, step, rest in cursors:
            self.assertEqual(cursor.remove(), 0)
            if keeps_position:
                self.assertEqual(cursor.get_key(), key)
            else:
                self.assertRaisesWithMessage(
                    wiredtiger.WiredTigerError, lambda: cursor.get_key(), '/requires key be set/')
            walked = []
            while step() == 0:
                walked.append(cursor.get_key())
            self.assertEqual(walked, rest)
        self.assertEqual(self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted),
            removed + len(cursors))
        self.session_follow.rollback_transaction()
        for cursor, _, _ in cursors:
            cursor.close()

    def test_remove_key_removed_by_other_txn(self):
        self.check_remove_removed_key(
            'ingest_key', 'ingest_val', lambda: self.remove_key('ingest_key'))

    def test_remove_truncated_key(self):
        self.check_remove_removed_key(
            'stable_key', 'stable_val', lambda: self.truncate_key('stable_key'))

    # The cursors are positioned on another transaction's uncommitted insert of a truncated key,
    # which rolls back. With nothing left on the key, the cursors lose their position.
    def test_remove_rolled_back_key(self):
        self.truncate_key('stable_key')
        session = self.conn_follow.open_session('')
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor['stable_key'] = 'uncommitted_val'
        self.check_remove_removed_key('stable_key', 'uncommitted_val', session.rollback_transaction,
            read_uncommitted=True, keeps_position=False)
        session.close()

    # A concurrent update to the key, committed later or prepared, makes a blind remove fail with a
    # write conflict without opening stable, and the key keeps the other transaction's outcome.
    def test_conflicts(self):
        other = self.conn_follow.open_session('')
        other_cursor = other.open_cursor(self.uri)
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
        ts = 30
        for key, value in (('ingest_key', 'ingest_val'), ('stable_key', 'stable_val')):
            for prepare in (False, True):
                ts += 10
                other.begin_transaction()
                other_cursor[key] = 'other_val'
                if prepare:
                    other.prepare_transaction('prepare_timestamp=' + self.timestamp_str(ts) +
                        ',prepared_id=' + self.prepared_id_str(1))

                opens = self.get_follower_stat(stat.conn.layered_curs_open_stable)
                self.session_follow.begin_transaction()
                cursor.set_key(key)
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.remove(),
                    '/conflict between concurrent operations/')
                self.session_follow.rollback_transaction()
                self.assertEqual(self.get_follower_stat(stat.conn.layered_curs_open_stable), opens)

                if prepare:
                    other.rollback_transaction()
                    self.assertEqual(self.search(self.session_follow, key), (0, value))
                else:
                    other.commit_transaction('commit_timestamp=' + self.timestamp_str(ts + 1))
                    self.assertEqual(self.search(self.session_follow, key), (0, 'other_val'))
                    with self.transaction(session=other, commit_timestamp=ts + 2):
                        other_cursor[key] = value
        cursor.close()
        other.close()

    # Under a read timestamp older than a key's stable value, a blind remove succeeds whether or not
    # the cursor already reads stable: conflicts come from ingest alone.
    def test_read_timestamp_before_stable_value(self):
        for stable_open in (False, True):
            cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
            if stable_open:
                cursor.set_key('anchor')
                self.assertEqual(cursor.search(), 0)
                cursor.reset()
            with self.transaction(session=self.session_follow, read_timestamp=5, rollback=True):
                cursor.set_key('stable_key')
                self.assertEqual(cursor.remove(), 0)
            cursor.close()

    # A read after a blind remove in the same transaction moves a cursor reading an older checkpoint
    # to the follower's newest one.
    def test_read_after_remove_sees_newest_checkpoint(self):
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
        cursor.set_key('anchor')
        self.assertEqual(cursor.search(), 0)
        cursor.reset()

        leader_cursor = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=15):
            leader_cursor['z_stable'] = 'z_val_15'
        leader_cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(15))
        self.session.checkpoint()
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

        with self.transaction(session=self.session_follow, rollback=True):
            cursor.set_key('absent_key')
            self.assertEqual(cursor.remove(), 0)
            cursor.set_key('z_stable')
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), 'z_val_15')
        cursor.close()

    # A key removed blindly from stable stays removed once the follower steps up.
    def test_remove_stable_key_then_step_up(self):
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
        opens = self.get_follower_stat(stat.conn.layered_curs_open_stable)
        with self.transaction(session=self.session_follow, commit_timestamp=30):
            cursor.set_key('stable_key')
            self.assertEqual(cursor.remove(), 0)
        self.assertEqual(self.get_follower_stat(stat.conn.layered_curs_open_stable), opens)
        cursor.close()

        self.ignoreStdoutPattern('Picking up the same checkpoint')
        self.disagg_switch_follower_and_leader(self.conn_follow)
        self.assertEqual(self.search(self.session_follow, 'stable_key')[0], wiredtiger.WT_NOTFOUND)
        self.assertEqual(self.search(self.session_follow, 'anchor'), (0, 'anchor'))

    # Without the configuration, removing a missing key on a follower fails as usual.
    def test_not_blind(self):
        blind = self.get_follower_stat(stat.conn.layered_curs_blind_remove_follower)
        cursor = self.session_follow.open_cursor(self.uri)
        for key, config, expected in (('absent_1', 'blind_remove=true', 0),
                ('absent_2', 'blind_remove=false', wiredtiger.WT_NOTFOUND)):
            cursor.reconfigure(config)
            with self.transaction(session=self.session_follow, rollback=True):
                cursor.set_key(key)
                self.assertEqual(cursor.remove(), expected)
        cursor.close()
        self.assertEqual(
            self.get_follower_stat(stat.conn.layered_curs_blind_remove_follower), blind + 1)

    # Removing a key outside the cursor bounds fails with an invalid argument error and leaves the
    # key as it was, whether it lives in stable, lives in ingest, is already removed, or is missing.
    # Removing a missing key inside the bounds succeeds.
    def test_bounds(self):
        start = self.session_follow.open_cursor(self.uri)
        stop = self.session_follow.open_cursor(self.uri)
        start.set_key('anchor')
        stop.set_key('anchor')
        with self.transaction(session=self.session_follow, commit_timestamp=25):
            self.session_follow.truncate(None, start, stop, None)
        start.close()
        stop.close()
        cursor = self.session_follow.open_cursor(self.uri)
        with self.transaction(session=self.session_follow, commit_timestamp=26):
            cursor['b_removed'] = 'b_val'
        with self.transaction(session=self.session_follow, commit_timestamp=27):
            cursor.set_key('b_removed')
            self.assertEqual(cursor.remove(), 0)
        cursor.close()

        removed = self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted)
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')
        cursor.set_key('m')
        cursor.bound('bound=lower')
        cursor.set_key('y')
        cursor.bound('bound=upper')
        with self.transaction(session=self.session_follow, rollback=True):
            for key in ('a_absent', 'anchor', 'b_removed', 'ingest_key', 'z_stable'):
                cursor.set_key(key)
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.remove(),
                    '/blind_remove cannot remove a key outside the cursor bounds/')
        self.assertEqual(self.search(self.session_follow, 'ingest_key'), (0, 'ingest_val'))
        self.assertEqual(self.search(self.session_follow, 'z_stable'), (0, 'z_val'))

        with self.transaction(session=self.session_follow, rollback=True):
            cursor.set_key('n_absent')
            self.assertEqual(cursor.remove(), 0)
        cursor.close()
        self.assertEqual(
            self.get_follower_stat(stat.conn.layered_curs_blind_remove_ingest_deleted), removed)
