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

# A scan returns readable keys before a prepared insert and continues after rollback exhausts ingest.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios


@disagg_test_class
class test_layered_prepare08(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    def test_compare_guard_after_ingest_exhaust(self):
        """
        Return key 1 before conflicting on prepared key 2, then return keys 3 and 5
        after rollback without resetting the cursor or restarting its read transaction.
        """
        # --- Phase 1: leader commits keys [1, 3, 5] and takes a checkpoint ----------
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c[1] = 'value_1'
        c[3] = 'value_3'
        c[5] = 'value_5'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        # --- Phase 2: open follower and advance it to the leader checkpoint ----------
        # The follower's stable btree now contains keys 1, 3, 5.
        conn_f = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.disagg_advance_checkpoint(conn_f)

        # --- Phase 3: write exactly one prepared key (key 2) into the follower ingest --
        # prepare_timestamp=25 falls within the read_timestamp=30 window, so a reader at
        # ts=30 encounters WT_PREPARE_CONFLICT rather than skipping the prepared record.
        session_prep = conn_f.open_session()
        cursor_prep = session_prep.open_cursor(self.uri)
        session_prep.begin_transaction()
        cursor_prep[2] = 'prepared_value'
        session_prep.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(25) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prep.close()

        # Return the readable prefix before reaching the prepared insert.
        session_r = conn_f.open_session()
        cursor_r = session_r.open_cursor(self.uri)
        session_r.begin_transaction('read_timestamp=' + self.timestamp_str(30))

        self.assertEqual(cursor_r.next(), 0)
        prefix = [(cursor_r.get_key(), cursor_r.get_value())]
        self.assertEqual(prefix, [(1, 'value_1')])
        for _ in range(2):
            self.assertRaisesException(wiredtiger.WiredTigerError, cursor_r.next,
                                       '/WT_PREPARE_CONFLICT/')

        # --- Phase 5: roll back the prepared transaction ----------------------------
        # Key 2 is now absent from the ingest btree.
        session_prep.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(40))
        session_prep.close()

        # Continue the same cursor after rollback exhausts ingest.
        expected_suffix = [(3, 'value_3'), (5, 'value_5')]
        suffix = []
        for pair in expected_suffix:
            self.assertEqual(cursor_r.next(), 0)
            suffix.append((cursor_r.get_key(), cursor_r.get_value()))
            self.assertEqual(suffix[-1], pair)
        self.assertEqual(cursor_r.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(suffix, expected_suffix)
        self.assertEqual(prefix + suffix, [(1, 'value_1'), (3, 'value_3'), (5, 'value_5')])

        session_r.rollback_transaction()
        cursor_r.close()
        session_r.close()
        conn_f.close()
