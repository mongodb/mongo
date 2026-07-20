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

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Regression coverage for WT-15189: next_random on a layered table must
# return WT_NOTFOUND rather than spin when every reachable row is a
# tombstone. Covers the all-deleted-in-ingest case (stable empty) and the
# all-deleted-scattered case (tombstones in both ingest and stable).

@disagg_test_class
@wttest.skip_for_hook("tiered", "Cannot run tiered storage in disagg mode")
class test_layered_cursor21(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri=f'layered:{test_name}')),
        ('table', dict(uri=f'table:{test_name}')),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)

    nitems = 1000

    @staticmethod
    def key(n):
        return f'{n:04d}'

    def session_create_config(self):
        cfg = 'key_format=S,value_format=S'
        if self.uri.startswith('table'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    # Create the table on the leader and (optionally) populate the stable
    # table, then reopen as a follower.
    def setup_follower(self, populate):
        self.session.create(self.uri, self.session_create_config())
        if populate:
            cursor = self.session.open_cursor(self.uri)
            for i in range(self.nitems):
                self.session.begin_transaction()
                cursor[self.key(i)] = 'value'
                self.session.commit_transaction(
                    f'commit_timestamp={self.timestamp_str(10)}')
            cursor.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        follower_config = (
            'disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        )
        self.reopen_conn(config=follower_config)

    def truncate_range(self, start, stop, ts):
        c1 = self.session.open_cursor(self.uri)
        c1.set_key(self.key(start))
        c2 = self.session.open_cursor(self.uri)
        c2.set_key(self.key(stop))
        self.session.begin_transaction()
        self.session.truncate(None, c1, c2, None)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        c1.close()
        c2.close()

    def remove_range(self, start, stop, ts):
        cursor = self.session.open_cursor(self.uri, None, 'overwrite=false')
        self.session.begin_transaction()
        for i in range(start, stop + 1):
            cursor.set_key(self.key(i))
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def assert_random_notfound(self):
        cursor = self.session.open_cursor(self.uri, None, 'next_random=true')
        self.session.begin_transaction()
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

    # Stable is empty; insert rows on the follower (they land in ingest) and
    # truncate them all. next_random must return WT_NOTFOUND, not spin.
    def test_all_tombstones_ingest_only(self):
        self.setup_follower(populate=False)

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(self.nitems):
            cursor[self.key(i)] = 'value'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor.close()

        self.truncate_range(0, self.nitems - 1, 30)
        self.assert_random_notfound()

    # Same as test_all_tombstones_ingest_only but deletes via per-key remove()
    # instead of session.truncate(), since the two paths produce tombstones
    # through different code.
    def test_all_tombstones_ingest_only_remove(self):
        self.setup_follower(populate=False)

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(self.nitems):
            cursor[self.key(i)] = 'value'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor.close()

        self.remove_range(0, self.nitems - 1, 30)
        self.assert_random_notfound()

    # Tombstones genuinely scattered across both constituents:
    #   - On the leader, insert 0..2*nitems-1 then truncate the lower half so
    #     stable holds tombstones for 0..nitems-1 and live rows for the upper
    #     half. Checkpoint.
    #   - Reopen as follower and truncate the upper half so ingest holds
    #     tombstones covering nitems..2*nitems-1.
    # Net: no visible rows, with tombstones residing in both ingest and
    # stable. next_random must return WT_NOTFOUND.
    def test_all_tombstones_scattered(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        for i in range(2 * self.nitems):
            self.session.begin_transaction()
            cursor[self.key(i)] = 'value'
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor.close()

        # Leader-side truncate: tombstones land in stable on checkpoint.
        self.truncate_range(0, self.nitems - 1, 20)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        self.session.checkpoint()

        follower_config = (
            'disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        )
        self.reopen_conn(config=follower_config)

        # Follower-side truncate: tombstones land in ingest.
        self.truncate_range(self.nitems, 2 * self.nitems - 1, 30)
        self.assert_random_notfound()

    # Same as test_all_tombstones_scattered but using per-key remove() on both
    # the leader and follower sides.
    def test_all_tombstones_scattered_remove(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        for i in range(2 * self.nitems):
            self.session.begin_transaction()
            cursor[self.key(i)] = 'value'
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor.close()

        # Leader-side remove: tombstones land in stable on checkpoint.
        self.remove_range(0, self.nitems - 1, 20)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        self.session.checkpoint()

        follower_config = (
            'disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        )
        self.reopen_conn(config=follower_config)

        # Follower-side remove: tombstones land in ingest.
        self.remove_range(self.nitems, 2 * self.nitems - 1, 30)
        self.assert_random_notfound()
