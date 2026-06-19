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
from wtscenario import make_scenarios

# Test that cursor operations on a layered cursor after step-up work correctly
# when the cursor was reset on the follower before the step-up.
#
# Two checkpoints with distinct content are created on the leader:
#   ckpt1 (TS=1): keys "1", "3", "5"
#   ckpt2 (TS=2): adds "7", "9" and updates "1" to a new value
@disagg_test_class
class test_layered_stepup05(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f"layered:{test_name}"

    conn_base_config = 'statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    disagg_storages = gen_disagg_storages(disagg_only=True)

    # Timestamps for the two checkpoints.
    ckpt1_ts = 1
    ckpt2_ts = 2

    # Data written at each checkpoint. ckpt2 re-uses key "1" so that
    # the two checkpoints have a different value for the same key.
    ckpt1_data    = {'1': 'ckpt1_val1', '3': 'ckpt1_val3', '5': 'ckpt1_val5'}
    ckpt2_updates = {'1': 'ckpt2_val1', '7': 'ckpt2_val7', '9': 'ckpt2_val9'}

    def _do_next(self, cursor, ckpt1):
        # After a reset, next() moves to the first (smallest) key.
        # Key "1" exists in both checkpoints; only its value differs.
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), '1')
        self.assertEqual(cursor.get_value(), 'ckpt1_val1' if ckpt1 else 'ckpt2_val1')

    def _do_prev(self, cursor, ckpt1):
        # After a reset, prev() moves to the last (largest) key.
        # ckpt1 ends at "5"; ckpt2 ends at "9".
        self.assertEqual(cursor.prev(), 0)
        if ckpt1:
            self.assertEqual(cursor.get_key(), '5')
            self.assertEqual(cursor.get_value(), 'ckpt1_val5')
        else:
            self.assertEqual(cursor.get_key(), '9')
            self.assertEqual(cursor.get_value(), 'ckpt2_val9')

    def _do_search(self, cursor, ckpt1):
        # Key "7" was added at ckpt2, so it is invisible at ckpt1.
        cursor.set_key('7')
        if ckpt1:
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), 'ckpt2_val7')

    def _do_search_near(self, cursor, ckpt1):
        # Searching for "7" (added at ckpt2, absent in ckpt1).
        cursor.set_key('7')
        exact = cursor.search_near()
        if ckpt1:
            self.assertEqual(exact, -1)
            self.assertEqual(cursor.get_key(), '5')
            self.assertEqual(cursor.get_value(), 'ckpt1_val5')
        else:
            self.assertEqual(exact, 0)
            self.assertEqual(cursor.get_key(), '7')
            self.assertEqual(cursor.get_value(), 'ckpt2_val7')

    # Scenarios
    operations = [
        ('next',        dict(op=_do_next)),
        ('prev',        dict(op=_do_prev)),
        ('search',      dict(op=_do_search)),
        ('search_near', dict(op=_do_search_near)),
    ]

    # Transaction and read-timestamp variants.
    txn_variants = [
        ('no_txn',            dict(use_txn=False, read_ts=None)),
        ('txn',               dict(use_txn=True,  read_ts=None)),
        ('txn_read_ts_ckpt1', dict(use_txn=True,  read_ts=1)),
        ('txn_read_ts_ckpt2', dict(use_txn=True,  read_ts=2)),
    ]

    scenarios = make_scenarios(disagg_storages, operations, txn_variants)

    def _sees_ckpt1(self):
        return self.read_ts == 1

    def _run_cursor_op(self, session, cursor):
        """Begin transaction if requested, dispatch self.op, then commit."""
        if self.use_txn:
            ts_cfg = ''
            if self.read_ts:
                ts_cfg = 'read_timestamp=' + self.timestamp_str(self.read_ts)
            session.begin_transaction(ts_cfg)

        # op is stored as a plain function in the instance dict (not auto-bound),
        # so self must be passed explicitly.
        self.op(self, cursor, self._sees_ckpt1())

        if self.use_txn:
            session.commit_transaction()

    def _write_checkpoint(self, data, ts):
        """Commit data at ts on the leader, advance the stable timestamp, and checkpoint."""
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for k, v in data.items():
            cursor[k] = v
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

    def test_cursor_reset_then_stepup(self):
        # Write ckpt1 data on the leader and take the first checkpoint.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self._write_checkpoint(self.ckpt1_data, self.ckpt1_ts)

        # Create a follower and advance it to ckpt1.
        conn_follow = self.wiredtiger_open('follower',self.extensionsConfig() + ',create,' + self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        self.disagg_advance_checkpoint(conn_follow)

        # Open a cursor on the follower and prime the stable cursor by
        # iterating once, then reset it so it is ready for use after step-up.
        cursor_follow = session_follow.open_cursor(self.uri)
        self.assertEqual(cursor_follow.next(), 0)
        cursor_follow.reset()

        # Write ckpt2 data on the leader: two new keys and an update to "1".
        self._write_checkpoint(self.ckpt2_updates, self.ckpt2_ts)

        # Advance the follower to ckpt2 so it sees the full dataset.
        self.disagg_advance_checkpoint(conn_follow)

        # Close the leader and step up the follower.
        self.session.close()
        self.close_conn()
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Run the cursor operation (with optional transaction) and verify
        # that results match the expected checkpoint view.
        self._run_cursor_op(session_follow, cursor_follow)

        session_follow.close()
        conn_follow.close()
