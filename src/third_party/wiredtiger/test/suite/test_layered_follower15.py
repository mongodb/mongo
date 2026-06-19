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

# Test MODIFY dependent on a GC-eligible base value.
#
# Covers reads via cursor.next and cursor.search, and the case where
# the on-disk base is not yet GC-eligible and must remain readable
# across its full visibility window.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_follower15(wttest.WiredTigerTestCase):
    test_name = __qualname__
    base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = base_config + 'disaggregated=(role="leader")'
    conn_config_follower = base_config + 'disaggregated=(role="follower")'

    uri = f'layered:{test_name}'
    ingest_uri = f'file:{test_name}.wt_ingest'
    create_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_follow = None
    session_follow = None

    SINGLE_KEY = 1

    def create_follower_and_tables(self):
        self.conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session()
        self.session.create(self.uri, self.create_config)
        self.session_follow.create(self.uri, self.create_config)

    def commit_items(self, session, items, commit_ts):
        c = session.open_cursor(self.uri)
        session.begin_transaction()
        for key, value in items.items():
            c[key] = value
        session.commit_transaction(
            f'commit_timestamp={self.timestamp_str(commit_ts)}')
        c.close()

    def commit_on_leader(self, items, commit_ts):
        self.commit_items(self.session, items, commit_ts)

    def commit_on_follower(self, items, commit_ts):
        self.commit_items(self.session_follow, items, commit_ts)

    def commit_on_both(self, items, commit_ts):
        self.commit_on_leader(items, commit_ts)
        self.commit_on_follower(items, commit_ts)

    def modify_on_both(self, key, mods, commit_ts):
        for session in (self.session, self.session_follow):
            c = session.open_cursor(self.uri)
            session.begin_transaction()
            c.set_key(key)
            self.assertEqual(c.modify(mods), 0)
            session.commit_transaction(
                f'commit_timestamp={self.timestamp_str(commit_ts)}')
            c.close()

    def set_stable_on_both(self, stable_ts):
        ts = f'stable_timestamp={self.timestamp_str(stable_ts)}'
        self.conn.set_timestamp(ts)
        self.conn_follow.set_timestamp(ts)

    def checkpoint_and_advance_follower(self, evict_key):
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.force_evict(self.conn_follow, self.uri, evict_key)

    def force_evict(self, conn, uri, key):
        session_evict = conn.open_session('debug=(release_evict_page)')
        evict_cursor = session_evict.open_cursor(uri)
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.close()
        session_evict.close()

    def setup_single_key_chain(self, base_value, full_value, make_orphan):
        """
        Single-key timeline through force eviction on the follower's ingest
        btree. Builds the update chain (newest to oldest):

           [  STANDARD(full_value) @30 -> MODIFY @20 -> base_value @10  ]
                                                     ^                 ^
                                                     |                 |
                                          prune_timestamp@11           |
                                          (make_orphan=True)           |
                                                     |                 |
                                                     |        prune_timestamp@8
                                                     |        (make_orphan=False)

        Returns the value visible at read timestamp 25.
        """
        self.create_follower_and_tables()

        key = self.SINGLE_KEY
        self.commit_on_both({key: base_value}, 10)
        self.force_evict(self.conn_follow, self.uri, key)

        if make_orphan:
            self.set_stable_on_both(11)
        else:
            self.set_stable_on_both(8)

        # Before MODIFY: "v1". After MODIFY: "v1" + "X" * 100.
        # Make the post-modify value long enough so it's not converted to a STANDARD.
        self.assertEqual(len(base_value), 2)
        self.modify_on_both(key, [wiredtiger.Modify('X' * 100, 2, 100)], 20)
        self.commit_on_both({key: full_value}, 30)

        # Sets prune_timestamp through checkpoint.
        self.checkpoint_and_advance_follower(key)

        return base_value + 'X' * 100

    def teardown_follower_and_checkpoint_leader(self):
        # Drop the follower before tearDown to avoid a verifyLayered
        # contention on a follower that still has the ingest btree open.
        self.session_follow.close()
        self.conn_follow.close()

        # The stable timestamp was pinned to control prune_timestamp, which
        # leaves the leader with uncheckpointed dirty data, which verifyLayered()
        # in tearDown can't handle. Advance stable and take a final checkpoint.
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(100)}')
        self.session.checkpoint()

    def test_modify_base_value_not_pruned(self):
        expected = self.setup_single_key_chain('v1', 'v2', make_orphan=True)

        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(25)}')
        cursor = self.session_follow.open_cursor(self.ingest_uri)
        # Tests cursor.next()
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 1)
        self.assertEqual(cursor.get_value(), expected)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)

        # Tests cursor.search()
        cursor.set_key(self.SINGLE_KEY)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), self.SINGLE_KEY)
        self.assertEqual(cursor.get_value(), expected)

        cursor.close()
        self.session_follow.rollback_transaction()

        self.teardown_follower_and_checkpoint_leader()

    def setup_multi_key_orphan_modify_precondition(self, num_keys, target_key):
        """
        Multi-key variant of setup_orphan_modify_precondition.

        Inserts K=1..num_keys at ts=10 so all keys share a single leaf
        page on the follower's ingest btree, then puts the
        [STANDARD -> MODIFY -> BASE_VALUE] chain on only target_key.

        Returns the expected value when target_key's MODIFY is
        correctly reconstructed against target_key's own on-disk value.
        """
        self.create_follower_and_tables()

        def initial_value(k):
            # Tag the key so cross-key contamination is visible from get_value().
            return 'v' + str(k)

        self.commit_on_leader({target_key: initial_value(target_key)}, 10)
        # Neighbors are NOT written on the leader; they live in the ingest btree only.
        self.commit_on_follower(
            {k: initial_value(k) for k in range(1, num_keys + 1)}, 10)
        self.force_evict(self.conn_follow, self.uri, 1)

        # Build two STANDARD updates on each neighbor.
        self.commit_on_follower(
            {k: 'u' + str(k)
             for k in range(1, num_keys + 1) if k != target_key}, 11)
        self.commit_on_follower(
            {k: 'w' + str(k)
             for k in range(1, num_keys + 1) if k != target_key}, 20)

        self.set_stable_on_both(11)
        self.modify_on_both(target_key, [wiredtiger.Modify('X' * 100, 2, 100)], 20)
        self.commit_on_both({target_key: 'u' + str(target_key)}, 30)

        self.checkpoint_and_advance_follower(target_key)
        return initial_value(target_key) + 'X' * 100

    def test_modify_multi_key_no_cross_key_contamination(self):
        """
        Tests MODIFY reconstruction does not read neighbor key's value
        as its base and apply the delta to a wrong base value.
        """
        target_key = 5
        num_keys = 8
        expected = self.setup_multi_key_orphan_modify_precondition(
            num_keys=num_keys, target_key=target_key)

        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(25)}')
        cursor = self.session_follow.open_cursor(self.ingest_uri)

        # Probe a few neighbors first to confirm the page is in the
        # multi-key state the test requires. If neighbors return
        # WT_NOTFOUND, the eviction reconcile collapsed the page to
        # entries==0 and the test is unable to demonstrate cross-key
        # contamination.
        neighbors_found = False
        for k in (target_key - 1, target_key + 1,
                  target_key - 2, target_key + 2):
            cursor.set_key(k)
            if cursor.search() == 0:
                neighbors_found = True
                break
        self.assertTrue(
            neighbors_found,
            f'Multi-key precondition not established: no neighbor of '
            f'target_key={target_key} survives on the rebuilt ingest '
            f'page (page appears pruned).')

        cursor.set_key(target_key)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_key(), target_key)
        actual = cursor.get_value()

        tag = 'v' + str(target_key)
        self.assertTrue(
            actual.startswith(tag),
            f'Cross-key contamination on target_key={target_key}: '
            f'returned value has tag {actual[:2]}, expected {tag}.')
        self.assertEqual(actual, expected)

        cursor.close()
        self.session_follow.rollback_transaction()
        self.teardown_follower_and_checkpoint_leader()

    def test_modify_respects_visibility(self):
        """
        Tests that visibility is respected when the base value is not GC-eligible.
        """
        v0 = 'v0'
        v2 = 'v2'
        modified = self.setup_single_key_chain(v0, v2, make_orphan=False)

        # v0 visible @15.
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(15)}')
        cursor = self.session_follow.open_cursor(self.ingest_uri)
        cursor.set_key(self.SINGLE_KEY)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), v0)
        cursor.close()
        self.session_follow.rollback_transaction()

        # MODIFY visible @25.
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(25)}')
        cursor = self.session_follow.open_cursor(self.ingest_uri)
        cursor.set_key(self.SINGLE_KEY)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), modified)
        cursor.close()
        self.session_follow.rollback_transaction()

        # v2 visible @30.
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(30)}')
        cursor = self.session_follow.open_cursor(self.ingest_uri)
        cursor.set_key(self.SINGLE_KEY)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), v2)
        cursor.close()
        self.session_follow.rollback_transaction()

        self.teardown_follower_and_checkpoint_leader()
