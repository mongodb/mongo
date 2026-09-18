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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_verify_disagg06.py
#    Verify a data-store leaf whose reconstructed content outruns its parent's time
# aggregate does not report corruption on a disaggregated follower.
#
# A leader whose oldest timestamp has moved past a tombstone drops the deleted key
# entirely the next time it rebuilds the page from its base image and deltas, so the
# checkpoint it takes afterward carries a parent aggregate that no longer accounts
# for that key's start time. A follower picking up only that checkpoint, whose own
# oldest timestamp has never advanced past the tombstone -- the common case being
# one that was never set at all -- rebuilds the same base image and deltas without
# dropping the key, and its reconstructed leaf holds a cell whose start time
# predates what the checkpoint's own parent aggregate for that leaf claims is its
# floor.
#
# The follower only ever picks up this one, latest checkpoint -- it never adopts an
# earlier one first -- so the scenario stays valid regardless of whether checkpoint
# pick-up is later made to enforce oldest timestamp ordering between successive
# checkpoints a follower adopts.
@disagg_test_class
class test_verify_disagg06(wttest.WiredTigerTestCase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = ('disaggregated=(role="leader"),cache_size=20MB,precise_checkpoint=true,'
                   'statistics=(all),'
                   'page_delta=(delta_pct=100,leaf_page_delta=true,internal_page_delta=false)')
    conn_config_follower = ('disaggregated=(role="follower"),cache_size=20MB,statistics=(all),'
                            'page_delta=(delta_pct=100,leaf_page_delta=true,internal_page_delta=false)')

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'
    uri = f'layered:{test_name}'
    nitems = 10
    value = 'a' * 200

    def evict(self, key, uri, conn=None):
        # Force the page holding key out of cache so the next access rebuilds it
        # from its base image and deltas under whatever visibility the reading
        # connection has.
        c = conn if conn is not None else self.conn
        s = c.open_session('debug=(release_evict_page)')
        cursor = s.open_cursor(uri, None, None)
        s.begin_transaction()
        cursor.set_key(key)
        cursor.search_near()
        cursor.close()
        s.rollback_transaction()
        s.close()

    def test_follower_leaf_content_outruns_leader_aggregate(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.table_cfg)
        cursor = self.session.open_cursor(self.uri, None, None)

        # Key 0 is inserted and deleted first, at low timestamps. The other keys
        # are inserted afterward, at a higher timestamp: their own inserts must
        # stay behind wherever oldest lands next, or their start times get
        # cleared too (an insert whose visibility is global gets its own start
        # cleared, independent of any delete), which would mask the effect
        # being tested for.
        self.session.begin_transaction()
        cursor[str(0)] = self.value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.begin_transaction()
        cursor.set_key(str(0))
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(15))
        self.session.checkpoint()

        # Advance only the leader's oldest timestamp to a point strictly between
        # the delete (10) and the other keys' own insert (15): the tombstone
        # becomes globally visible and the key is dropped outright, while the
        # surviving keys' own start times are not yet globally visible and stay
        # untouched. Then force the leader to rebuild the page under that
        # visibility.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(12))
        self.evict(str(1), self.uri)

        # A write to the same leaf forces it to be reconciled from that
        # tombstone-free in-memory state, so the new delta's parent aggregate no
        # longer accounts for the dropped key's start time -- it now starts from
        # the surviving keys' real (unmodified) start of 15.
        self.session.begin_transaction()
        cursor[str(self.nitems)] = self.value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        cursor.close()

        # Only now does a follower pick up this checkpoint -- the latest one,
        # never having adopted the earlier one. Its own oldest timestamp is left
        # unset, well behind the tombstone (10) and the leader's current oldest
        # (12).
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                           self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        self.disagg_advance_checkpoint_and_wait(conn_follow)

        # The leader verifies cleanly: its own rebuild of the leaf agrees with
        # the aggregate it just wrote.
        self.verifyUntilSuccess(self.session)

        # The follower rebuilds the same base image and deltas under its own,
        # older visibility, so the tombstoned key survives with its original
        # start time -- outside what the leader's aggregate for this leaf
        # claims.
        self.verifyUntilSuccess(session_follow)

        session_follow.close()
        conn_follow.close()
