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

# test_layered85.py
#   Tests for mid-scan checkpoint advances on a follower cursor.
#
#   A new leader checkpoint arrives while a follower cursor is actively
#   iterating. The scan must complete without skipping or duplicating keys,
#   and must remain monotonically ordered.
#
#   Several tests assert that the layered_curs_advance_stable statistic
#   increments, confirming the checkpoint switch triggered during iteration.
#
#   A mid-scan checkpoint switch requires ALL of the following:
#     - A read timestamp is set on the active transaction.
#     - The cursor is positioned and actively iterating.
#     - A new checkpoint arrives after the cursor started iterating.

@disagg_test_class
class test_layered85(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    uri = 'layered:test_layered85'

    nkeys = 1000

    disagg_storages = gen_disagg_storages('test_layered85', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.ts = 1
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        config = "key_format=S,value_format=S"
        self.session.create(self.uri, config)
        self.session_follow.create(self.uri, config)

    @staticmethod
    def fmt_key(i):
        return f"{i:06d}"

    @staticmethod
    def fmt_val(i):
        return f"val_{i:06d}"

    def next_ts(self):
        self.ts += 1
        return self.ts

    def insert_leader(self, keys, values=None):
        """Insert keys on the leader. keys is a list of integers."""
        cursor = self.session.open_cursor(self.uri)
        for idx, k in enumerate(keys):
            key = self.fmt_key(k)
            val = values[idx] if values else self.fmt_val(k)
            self.session.begin_transaction()
            cursor[key] = val
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def remove_leader(self, keys):
        """Remove keys on the leader. keys is a list of integers."""
        cursor = self.session.open_cursor(self.uri)
        for k in keys:
            key = self.fmt_key(k)
            self.session.begin_transaction()
            cursor.set_key(key)
            cursor.remove()
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def insert_follower(self, keys, values=None):
        """Insert keys on the follower (local writes). keys is a list of integers."""
        cursor = self.session_follow.open_cursor(self.uri)
        for idx, k in enumerate(keys):
            key = self.fmt_key(k)
            val = values[idx] if values else self.fmt_val(k)
            self.session_follow.begin_transaction()
            cursor[key] = val
            self.session_follow.commit_transaction(
                f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def remove_follower(self, keys):
        """Remove keys on the follower (local deletes). keys is a list of integers."""
        cursor = self.session_follow.open_cursor(self.uri)
        for k in keys:
            key = self.fmt_key(k)
            self.session_follow.begin_transaction()
            cursor.set_key(key)
            cursor.remove()
            self.session_follow.commit_transaction(
                f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def checkpoint_and_advance(self):
        """Checkpoint on leader and advance follower."""
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.ts)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    def get_stat(self, stat_key):
        """Read a connection-level statistic from the follower."""
        stat_cursor = self.session_follow.open_cursor('statistics:')
        stat_cursor.set_key(stat_key)
        stat_cursor.search()
        val = stat_cursor.get_value()
        stat_cursor.close()
        # val is (description, type_string, value)
        return val[2]

    def begin_read_ts_txn(self):
        """Begin a transaction with a read timestamp on the follower."""
        read_ts = self.ts
        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(read_ts)}')
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(read_ts)}')

    # -----------------------------------------------------------------------
    # Test: Forward scan with a read timestamp stays monotonically ordered
    # across a mid-scan checkpoint advance.
    #
    # Checkpoint 1: even keys 0-998. Follower writes odd keys 1-999.
    # Begin transaction with read timestamp. Iterate 100 keys. Advance
    # checkpoint. Continue scanning. All keys must be in increasing order.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_during_scan_positioned_on_follower(self):
        """
        Forward and backward scans with a read timestamp each remain monotonically
        ordered across a mid-scan checkpoint advance.

        Setup: even keys 0-998 from leader, odd keys 1-999 from follower.
        Forward pass: iterate 100 keys, advance checkpoint, complete scan
        verify increasing order and that the checkpoint switch stat incremented.
        Backward pass (same cursor, reset): iterate 100 keys, advance checkpoint
        again, complete scan  verify decreasing order.
        """

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Checkpoint 1: even keys 0-998.
        self.insert_leader(list(range(0, self.nkeys, 2)))
        self.checkpoint_and_advance()

        # Follower writes odd keys.
        self.insert_follower(list(range(1, self.nkeys, 2)))

        self.begin_read_ts_txn()

        cursor = self.session_follow.open_cursor(self.uri)

        # --- Forward scan ---
        advance_stable_before = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)

        keys_before = []
        for _ in range(100):
            self.assertEqual(cursor.next(), 0)
            keys_before.append(cursor.get_key())

        # Advance checkpoint mid-forward-scan (keys > read_ts, so not visible).
        self.insert_leader(list(range(1000, 1100)))
        self.checkpoint_and_advance()

        keys_after = []
        while cursor.next() == 0:
            keys_after.append(cursor.get_key())

        advance_stable_after = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)
        self.assertGreater(advance_stable_after, advance_stable_before,
            "Checkpoint switch did not trigger during forward iteration")

        fwd_keys = keys_before + keys_after
        for i in range(len(fwd_keys) - 1):
            self.assertLess(fwd_keys[i], fwd_keys[i + 1],
                f"Forward out of order at {i}: {fwd_keys[i]} >= {fwd_keys[i + 1]}")

        # --- Backward scan ---
        cursor.reset()

        keys_before = []
        for _ in range(100):
            self.assertEqual(cursor.prev(), 0)
            keys_before.append(cursor.get_key())

        # Advance checkpoint mid-backward-scan (keys > read_ts, so not visible).
        self.insert_leader(list(range(1100, 1200)))
        self.checkpoint_and_advance()

        keys_after = []
        while cursor.prev() == 0:
            keys_after.append(cursor.get_key())

        bwd_keys = keys_before + keys_after
        for i in range(len(bwd_keys) - 1):
            self.assertGreater(bwd_keys[i], bwd_keys[i + 1],
                f"Backward out of order at {i}: {bwd_keys[i]} <= {bwd_keys[i + 1]}")

        cursor.close()
        self.session_follow.commit_transaction()

    def test_checkpoint_advance_during_bounded_scan_positioned_on_follower(self):
        """
        Bounded forward and backward scans with bounds [200, 800] and a mid-scan
        checkpoint advance each. All returned keys must be within bounds and in
        monotonic order. The forward pass also verifies the checkpoint switch stat
        incremented.
        """

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        self.insert_leader(list(range(0, self.nkeys, 2)))
        self.checkpoint_and_advance()

        self.insert_follower(list(range(1, self.nkeys, 2)))

        self.begin_read_ts_txn()

        lo, hi = 200, 800
        cursor = self.session_follow.open_cursor(self.uri)

        # --- Forward scan ---
        advance_stable_before = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)

        cursor.set_key(self.fmt_key(lo))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(hi))
        cursor.bound("bound=upper")

        keys_before = []
        for _ in range(50):
            self.assertEqual(cursor.next(), 0)
            keys_before.append(cursor.get_key())

        # Advance checkpoint mid-forward-scan (keys > read_ts, so not visible).
        self.insert_leader(list(range(1000, 1100)))
        self.checkpoint_and_advance()

        keys_after = []
        while cursor.next() == 0:
            keys_after.append(cursor.get_key())

        advance_stable_after = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)
        self.assertGreater(advance_stable_after, advance_stable_before,
            "Checkpoint switch did not trigger during bounded forward iteration")

        fwd_keys = keys_before + keys_after
        for i in range(len(fwd_keys) - 1):
            self.assertLess(fwd_keys[i], fwd_keys[i + 1],
                f"Forward out of order at {i}: {fwd_keys[i]} >= {fwd_keys[i + 1]}")
        for k in fwd_keys:
            self.assertGreaterEqual(k, self.fmt_key(lo))
            self.assertLessEqual(k, self.fmt_key(hi))

        # --- Backward scan ---
        # reset() clears bounds; re-apply before iterating.
        cursor.reset()
        cursor.set_key(self.fmt_key(lo))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(hi))
        cursor.bound("bound=upper")

        keys_before = []
        for _ in range(50):
            self.assertEqual(cursor.prev(), 0)
            keys_before.append(cursor.get_key())

        # Advance checkpoint mid-backward-scan (keys > read_ts, so not visible).
        self.insert_leader(list(range(1100, 1200)))
        self.checkpoint_and_advance()

        keys_after = []
        while cursor.prev() == 0:
            keys_after.append(cursor.get_key())

        bwd_keys = keys_before + keys_after
        for i in range(len(bwd_keys) - 1):
            self.assertGreater(bwd_keys[i], bwd_keys[i + 1],
                f"Backward out of order at {i}: {bwd_keys[i]} <= {bwd_keys[i + 1]}")
        for k in bwd_keys:
            self.assertGreaterEqual(k, self.fmt_key(lo))
            self.assertLessEqual(k, self.fmt_key(hi))

        cursor.close()
        self.session_follow.commit_transaction()

    def test_checkpoint_advance_during_scan_with_tombstones_on_follower(self):
        """
        Forward scan with deleted keys and a mid-scan checkpoint advance.
        Leader: even keys 0-998. Follower deletes even keys 400-600 and writes
        odd keys 1-999. Verifies monotonic order and that deleted keys are hidden
        after a checkpoint advance mid-scan.
        """

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Checkpoint 1: even keys 0-998.
        self.insert_leader(list(range(0, self.nkeys, 2)))
        self.checkpoint_and_advance()

        # Follower writes: odd keys added, even keys 400-600 deleted.
        self.insert_follower(list(range(1, self.nkeys, 2)))
        self.remove_follower(list(range(400, 601, 2)))

        self.begin_read_ts_txn()

        cursor = self.session_follow.open_cursor(self.uri)
        keys_before = []
        for _ in range(250):
            self.assertEqual(cursor.next(), 0)
            keys_before.append(cursor.get_key())

        # Checkpoint 2: leader adds more keys.
        self.insert_leader(list(range(1000, 1100)))
        self.checkpoint_and_advance()

        keys_after = []
        while cursor.next() == 0:
            keys_after.append(cursor.get_key())

        all_keys = keys_before + keys_after
        for i in range(len(all_keys) - 1):
            self.assertLess(all_keys[i], all_keys[i + 1],
                f"Out of order at {i}: {all_keys[i]} >= {all_keys[i + 1]}")

        # Deleted even keys 400-600 must not appear.
        tombstoned = set(self.fmt_key(k) for k in range(400, 601, 2))
        for k in all_keys:
            self.assertNotIn(k, tombstoned, f"Tombstoned key appeared: {k}")

        cursor.close()
        self.session_follow.commit_transaction()

    def test_multiple_checkpoint_advances_during_scan_on_follower(self):
        """
        Forward scan across multiple mid-scan checkpoint advances. Verifies
        monotonic order throughout and that at least one checkpoint switch occurred.
        """

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Checkpoint 1: keys 0-299.
        self.insert_leader(list(range(300)))
        self.checkpoint_and_advance()

        # Follower writes: odd keys 301-999.
        self.insert_follower(list(range(301, self.nkeys, 2)))

        self.begin_read_ts_txn()

        advance_stable_before = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)

        cursor = self.session_follow.open_cursor(self.uri)
        all_keys = []

        for _ in range(150):
            self.assertEqual(cursor.next(), 0)
            all_keys.append(cursor.get_key())

        # Checkpoint 2: add keys 300-599.
        self.insert_leader(list(range(300, 600)))
        self.checkpoint_and_advance()

        for _ in range(200):
            self.assertEqual(cursor.next(), 0)
            all_keys.append(cursor.get_key())

        # Checkpoint 3: add keys 600-999.
        self.insert_leader(list(range(600, self.nkeys)))
        self.checkpoint_and_advance()

        while cursor.next() == 0:
            all_keys.append(cursor.get_key())

        advance_stable_after = self.get_stat(wiredtiger.stat.conn.layered_curs_advance_stable)
        self.assertGreater(advance_stable_after, advance_stable_before,
            "checkpoint advance did not trigger during multi-checkpoint scan")

        for i in range(len(all_keys) - 1):
            self.assertLess(all_keys[i], all_keys[i + 1],
                f"Out of order at {i}: {all_keys[i]} >= {all_keys[i + 1]}")

        cursor.close()
        self.session_follow.commit_transaction()

    # -----------------------------------------------------------------------
    # Test that a forward scan remains complete when a key visible at scan
    # start is removed and a new checkpoint is applied mid-scan.
    # -----------------------------------------------------------------------

    def test_scan_completeness_after_checkpoint_removes_key_mid_scan(self):
        """
        Leader: even keys 0-998 (checkpointed). Follower: odd keys 1-999 (local writes).
        1. Begin a read transaction and scan forward past key 500.
        2. The leader removes an even key and checkpoints.
        3. Restart the scan with a new read timestamp that sees the delete.
        4. Continue scanning. All expected even keys after the removed key
           must still appear in the scan.
        """

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Checkpoint 1: even keys 0-998.
        even_keys = list(range(0, self.nkeys, 2))
        self.insert_leader(even_keys)
        self.checkpoint_and_advance()

        # Follower writes: odd keys 1-999.
        self.insert_follower(list(range(1, self.nkeys, 2)))

        # Read timestamp after the follower writes so they're visible.
        read_ts = self.ts

        # Begin transaction with read timestamp so the cursor can pick up
        # a new checkpoint mid-scan.
        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(read_ts)}')
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(read_ts)}')

        cursor = self.session_follow.open_cursor(self.uri)

        # Iterate forward past key 500.
        keys_before = []
        for _ in range(502):
            self.assertEqual(cursor.next(), 0)
            keys_before.append(cursor.get_key())

        last_key = keys_before[-1]
        last_key_int = int(last_key)
        key_to_remove = last_key_int + 1 if last_key_int % 2 == 1 else last_key_int

        # Commit the current transaction so we can start a new one later
        # with a higher read timestamp that sees the delete.
        self.session_follow.commit_transaction()

        # Remove the key on the leader and checkpoint.
        self.remove_leader([key_to_remove])
        self.checkpoint_and_advance()

        # Start a new transaction with a read timestamp after the remove,
        # so the deleted key is not visible in the new scan.
        new_read_ts = self.ts
        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(new_read_ts)}')
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(new_read_ts)}')

        # Continue scanning from the repositioned cursor.
        keys_after = []
        while cursor.next() == 0:
            keys_after.append(cursor.get_key())

        all_keys = keys_before + keys_after
        for i in range(len(all_keys) - 1):
            self.assertLess(all_keys[i], all_keys[i + 1],
                f"Out of order at {i}: {all_keys[i]} >= {all_keys[i + 1]}")

        # Even keys after the removed key must still appear.
        even_keys_after = [k for k in keys_after if int(k) % 2 == 0]
        self.assertGreater(len(even_keys_after), 0,
            f"No even keys found after the checkpoint advance. "
            f"Removed key: {key_to_remove}")

        # Specifically, even keys like 700, 702, ... 998 should be present
        # (they were not removed).
        expected_even_after = [self.fmt_key(k) for k in range(key_to_remove + 2, self.nkeys, 2)]
        found_even_after = set(even_keys_after)
        for k in expected_even_after:
            self.assertIn(k, found_even_after,
                f"Even key {k} missing after checkpoint advance")

        cursor.close()
        self.session_follow.commit_transaction()
