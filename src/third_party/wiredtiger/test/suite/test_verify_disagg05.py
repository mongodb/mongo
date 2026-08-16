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

# test_verify_disagg05.py
#    Verify a history store record whose key the data store no longer holds does not
#    report corruption on a disaggregated follower.
#
# A leader whose oldest timestamp has moved past a set of tombstones reconciles the
# deleted keys out of the page image entirely, and treats their leftover history store
# records as obsolete: its own history store cursor skips them because their stop times
# are globally visible to it. A follower has its own, much older, global visibility
# horizon. It reads the same checkpoint, does not consider those records obsolete, and
# so walks history store records whose keys the data store checkpoint no longer holds.

@disagg_test_class
class test_verify_disagg05(wttest.WiredTigerTestCase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Full images only, so a globally visible tombstone removes the key from the page
    # rather than leaving it behind as a tombstone in a delta. The small cache drives
    # eviction, which is what gets the older versions into the shared history store.
    conn_config = ('disaggregated=(role="leader"),cache_size=10MB,'
                   'eviction_dirty_target=5,eviction_dirty_trigger=10,'
                   'page_delta=(leaf_page_delta=false,internal_page_delta=false)')
    conn_config_follower = ('disaggregated=(role="follower"),cache_size=10MB,'
                            'page_delta=(leaf_page_delta=false,internal_page_delta=false)')

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'
    uri = f'layered:{test_name}'
    nitems = 3000
    valsz = 400

    def test_verify_follower_hs_key_missing_from_data_store(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.table_cfg)
        cursor = self.session.open_cursor(self.uri, None, None)

        # Several rounds of overwrites under eviction pressure, so the older versions
        # are pushed into the shared history store and captured by a checkpoint. History
        # store records written by a checkpoint's own reconciliation only land in the
        # next checkpoint, so a single one would leave the follower nothing to check.
        ts = 5
        for round in range(4):
            for i in range(self.nitems):
                self.session.begin_transaction()
                cursor[str(i)] = ('r%d_' % round) + 'x' * self.valsz
                self.session.commit_transaction(
                    'commit_timestamp=' + self.timestamp_str(ts))
            ts += 5
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts - 1))
            self.session.checkpoint()
        self.session.checkpoint()

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                           self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        self.disagg_advance_checkpoint(conn_follow)

        # At this point the follower is consistent and verifies cleanly.
        self.verifyUntilSuccess(session_follow)

        # Delete every key at a timestamp. A timestamped tombstone does not clear the
        # key's history store records.
        delete_ts = ts + 5
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(delete_ts))
        cursor.close()

        # Move the leader's horizon past the tombstones and checkpoint, so
        # reconciliation drops the keys from the page image.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(delete_ts + 5) +
                                ',stable_timestamp=' + self.timestamp_str(delete_ts + 5))
        self.session.checkpoint()
        self.session.checkpoint()
        self.session.checkpoint()

        self.disagg_advance_checkpoint(conn_follow)

        # The leader verifies cleanly: its horizon makes the leftover history store
        # records globally visible, so its history store cursor skips them.
        self.verifyUntilSuccess(self.session)

        # The follower reads the same checkpoint with a much older horizon, so it does
        # not skip them, and finds history store keys the data store no longer has.
        self.verifyUntilSuccess(session_follow)

        session_follow.close()
        conn_follow.close()
