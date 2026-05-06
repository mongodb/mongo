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

# test_layered101.py
#   After a prepared key triggers a conflict mid-scan and the prepared transaction is
#   later rolled back, the ingest cursor is left unpositioned. A subsequent scan must
#   skip the key comparison rather than asserting on the missing key.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios


@disagg_test_class
class test_layered101(wttest.WiredTigerTestCase):
    tablename = 'test_layered101'
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages('test_layered101', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    def test_compare_guard_after_ingest_exhaust(self):
        """
        Verify that a next() call does not assert when the ingest cursor is exhausted
        after a prepared key was rolled back mid-scan.

        Sequence:
          1. Stable btree has committed keys [1, 3, 5] from a leader checkpoint.
          2. Ingest btree has exactly one prepared key [2] (prepare_timestamp=15).
          3. cursor.next() at read_timestamp=20 triggers WT_PREPARE_CONFLICT: stable
             advances to key 1, ingest hits prepared key 2 (WT_CURSTD_KEY_INT cleared,
             ref still set; current_cursor left NULL).
          4. The prepared transaction is rolled back.  Key 2 vanishes from the ingest.
          5. cursor.next() again: the ingest cursor advances past the rolled-back slot
             and returns WT_NOTFOUND, leaving it unpositioned while the stable cursor
             is at key 1.
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
        # prepare_timestamp=15 falls within the read_timestamp=20 window, so a reader at
        # ts=20 encounters WT_PREPARE_CONFLICT rather than skipping the prepared record.
        session_prep = conn_f.open_session()
        cursor_prep = session_prep.open_cursor(self.uri)
        session_prep.begin_transaction()
        cursor_prep[2] = 'prepared_value'
        session_prep.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(15) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prep.close()

        # --- Phase 4: open read cursor and trigger the prepare conflict --------------
        session_r = conn_f.open_session()
        cursor_r = session_r.open_cursor(self.uri)
        session_r.begin_transaction('read_timestamp=' + self.timestamp_str(20))

        # First next(): fresh-start path positions stable at key 1 and advances ingest
        # to prepared key 2, returning WT_PREPARE_CONFLICT.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor_r.next())

        # --- Phase 5: roll back the prepared transaction ----------------------------
        # Key 2 is now absent from the ingest btree.
        session_prep.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(30))
        session_prep.close()

        # --- Phase 6: retry next()
        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 1)
        self.assertEqual(cursor_r.get_value(), 'value_1')

        # The remaining stable keys are returned in order.
        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 3)
        self.assertEqual(cursor_r.get_value(), 'value_3')

        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 5)
        self.assertEqual(cursor_r.get_value(), 'value_5')

        self.assertEqual(cursor_r.next(), wiredtiger.WT_NOTFOUND)

        session_r.rollback_transaction()
        cursor_r.close()
        session_r.close()
        conn_f.close()
