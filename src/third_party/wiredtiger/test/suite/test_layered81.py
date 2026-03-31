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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered81.py
#   Test that follower cursors see updated data after a new checkpoint is applied.
#
#   Key scenarios:
#   - Unpositioned cursor sees new data after checkpoint advance.
#   - Cursor preserves position correctly when checkpoint advances.
#   - With read timestamp, iteration triggers a checkpoint advance.
#   - Data added/updated/removed across checkpoints is visible after checkpoint advances.

@disagg_test_class
class test_layered81(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    uri = 'layered:test_layered81'

    nkeys = 1000

    disagg_storages = gen_disagg_storages('test_layered81', disagg_only=True)
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

    def do_checkpoint(self):
        """Checkpoint on leader and advance follower."""
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.ts)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    def scan_keys(self, session):
        """Return all keys in forward order."""
        cursor = session.open_cursor(self.uri)
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        cursor.close()
        return keys

    def scan_kv(self, session):
        """Return all (key, value) pairs in forward order."""
        cursor = session.open_cursor(self.uri)
        result = []
        while cursor.next() == 0:
            result.append((cursor.get_key(), cursor.get_value()))
        cursor.close()
        return result

    # -----------------------------------------------------------------------
    # Test: An existing cursor sees new data after checkpoint advance.
    #
    # Checkpoint 1: even keys 0-998. Open a cursor, scan, reset (unpositioned).
    # Checkpoint 2: all keys 0-999. The same cursor must see all 1000 keys.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_full_scan(self):

        even_keys = list(range(0, self.nkeys, 2))
        self.insert_leader(even_keys)
        self.do_checkpoint()

        # Open one cursor and verify it sees only even keys.
        cursor = self.session_follow.open_cursor(self.uri)
        expected_even = [self.fmt_key(i) for i in even_keys]
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, expected_even)
        cursor.reset()

        # Advance to a new checkpoint that adds odd keys.
        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_leader(odd_keys)
        self.do_checkpoint()

        # Trigger the cursor to pick up the new checkpoint via search, then verify full scan sees all 1000 keys.
        all_keys = [self.fmt_key(i) for i in range(self.nkeys)]
        cursor.set_key(self.fmt_key(0))
        self.assertEqual(cursor.search(), 0)
        cursor.reset()
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, all_keys)
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Updated values visible after checkpoint advance.
    #
    # Checkpoint 1: 1000 keys with original values.
    # Checkpoint 2: every 10th key updated with new value.
    # After checkpoint advance, follower should see the updated values.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_updated_value(self):

        all_keys = list(range(self.nkeys))
        self.insert_leader(all_keys)
        self.do_checkpoint()

        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(0))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(0))
        cursor.reset()

        # Update every 10th key on leader and checkpoint.
        update_keys = list(range(0, self.nkeys, 10))
        update_vals = [f"updated_{i:06d}" for i in update_keys]
        self.insert_leader(update_keys, values=update_vals)
        self.do_checkpoint()

        # After checkpoint advance, should see new values for updated keys.
        for i in update_keys:
            cursor.set_key(self.fmt_key(i))
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), f"updated_{i:06d}")
            cursor.reset()

        # Non-updated keys should retain original values.
        cursor.set_key(self.fmt_key(1))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(1))
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Deleted key disappears after checkpoint advance.
    #
    # Checkpoint 1: 1000 keys. Checkpoint 2: every 3rd key removed.
    # Follower should not see removed keys after checkpoint advance.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_deleted_key(self):

        all_keys = list(range(self.nkeys))
        self.insert_leader(all_keys)
        self.do_checkpoint()

        expected_all = [self.fmt_key(i) for i in all_keys]
        self.assertEqual(self.scan_keys(self.session_follow), expected_all)

        remove_keys = list(range(0, self.nkeys, 3))
        self.remove_leader(remove_keys)
        self.do_checkpoint()

        remaining = [self.fmt_key(i) for i in all_keys if i % 3 != 0]
        self.assertEqual(self.scan_keys(self.session_follow), remaining)

        # Verify each removed key returns WT_NOTFOUND via search.
        cursor = self.session_follow.open_cursor(self.uri)
        for i in remove_keys:
            cursor.set_key(self.fmt_key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Cursor positioned on a locally-written key can iterate forward
    # and see new data after a checkpoint advances.
    #
    # Checkpoint 1: keys 0-499. Follower writes 500-999 locally.
    # Cursor positioned on key 750. Checkpoint 2: adds key 1000.
    # Forward iteration from 750 must reach key 1000.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_positioned_on_local_key(self):

        stable_keys = list(range(500))
        self.insert_leader(stable_keys)
        self.do_checkpoint()

        follower_keys = list(range(500, self.nkeys))
        self.insert_follower(follower_keys)

        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(750))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(750))

        # Advance checkpoint: adds key 1000.
        self.insert_leader([1000])
        self.do_checkpoint()

        # Without resetting, verify the cursor can find new stable data.
        cursor.set_key(self.fmt_key(1000))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(1000))
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Checkpoint 1: even keys 0-998. Follower adds some odd keys locally.
    # Checkpoint 2: all keys 0-999. After advance, iteration shows all keys in order.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_interleaved(self):

        even_keys = list(range(0, self.nkeys, 2))
        self.insert_leader(even_keys)
        self.do_checkpoint()

        follower_odd = list(range(1, 200, 2))
        self.insert_follower(follower_odd)

        expected = sorted([self.fmt_key(i) for i in even_keys] +
                          [self.fmt_key(i) for i in follower_odd])
        self.assertEqual(self.scan_keys(self.session_follow), expected)

        all_odd = list(range(1, self.nkeys, 2))
        self.insert_leader(all_odd)
        self.do_checkpoint()

        all_keys = [self.fmt_key(i) for i in range(self.nkeys)]
        self.assertEqual(self.scan_keys(self.session_follow), all_keys)

    # -----------------------------------------------------------------------
    # Test: search_near after checkpoint advance finds new closer key.
    #
    # Checkpoint 1: 1000 keys missing key 500.
    # search_near(500) -> gets a neighbor.
    # Checkpoint 2: adds key 500. search_near(500) -> exact match.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_search_near(self):

        keys_without_500 = [i for i in range(self.nkeys) if i != 500]
        self.insert_leader(keys_without_500)
        self.do_checkpoint()

        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(500))
        exact = cursor.search_near()
        # 500 doesn't exist yet; nearest neighbor is either 499 or 501.
        self.assertIn(cursor.get_key(), [self.fmt_key(499), self.fmt_key(501)])
        self.assertNotEqual(exact, 0)
        if cursor.get_key() == self.fmt_key(499):
            self.assertEqual(exact, -1)
        else:
            self.assertEqual(exact, 1)
        cursor.reset()

        # Add 500 and checkpoint.
        self.insert_leader([500])
        self.do_checkpoint()

        # After checkpoint advance, search_near should find exact match.
        cursor.set_key(self.fmt_key(500))
        exact = cursor.search_near()
        self.assertEqual(exact, 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(500))
        self.assertEqual(cursor.get_value(), self.fmt_val(500))
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Read timestamp controls which checkpoint's data is visible.
    #
    # Checkpoint 1: keys 0-499. Checkpoint 2: keys 500-999.
    # A transaction at checkpoint 1's timestamp sees only keys 0-499.
    # A transaction at checkpoint 2's timestamp sees all 1000 keys.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_with_read_timestamp_iteration(self):

        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        first_half = list(range(0, 500))
        self.insert_leader(first_half)
        self.do_checkpoint()
        ts_after_ckpt1 = self.ts

        # Add second half and checkpoint.
        second_half = list(range(500, self.nkeys))
        self.insert_leader(second_half)
        self.do_checkpoint()
        ts_after_ckpt2 = self.ts

        cursor = self.session_follow.open_cursor(self.uri)

        # Read at checkpoint 1's timestamp: only keys 0-499 visible.
        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(ts_after_ckpt1)}')
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(ts_after_ckpt1)}')
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, [self.fmt_key(i) for i in first_half])
        cursor.reset()
        self.session_follow.commit_transaction()

        # Read at checkpoint 2's timestamp: all 1000 keys visible.
        self.conn_follow.set_timestamp(f'oldest_timestamp={self.timestamp_str(ts_after_ckpt2)}')
        self.session_follow.begin_transaction(
            f'read_timestamp={self.timestamp_str(ts_after_ckpt2)}')
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, [self.fmt_key(i) for i in range(self.nkeys)])
        cursor.close()
        self.session_follow.commit_transaction()

    # -----------------------------------------------------------------------
    # Test: Advance preserves bounds.
    #
    # 1000 keys, bounds [200, 800].
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_preserves_bounds(self):

        all_keys = list(range(self.nkeys))
        self.insert_leader(all_keys)
        self.do_checkpoint()

        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        # Scan within bounds: keys 200-800.
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        expected_bounded = [self.fmt_key(i) for i in range(200, 801)]
        self.assertEqual(keys, expected_bounded)

        # Reset clears bounds. Re-apply them.
        cursor.reset()
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        # Add more data outside bounds and checkpoint.
        self.insert_leader([1001, 1002])
        self.do_checkpoint()

        cursor.set_key(self.fmt_key(500))
        self.assertEqual(cursor.search(), 0)
        cursor.reset()

        # Re-apply bounds after reset.
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        # After checkpoint advance, bounds should be in effect. 1001 and 1002 are outside.
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, expected_bounded)
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Advance with bounds and new data inside bounds.
    #
    # Even keys first, odd keys added in checkpoint 2, bounds [200, 800].
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_bounds_new_data_inside(self):

        even_keys = list(range(0, self.nkeys, 2))
        self.insert_leader(even_keys)
        self.do_checkpoint()

        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        expected_even_bounded = [self.fmt_key(i) for i in range(200, 801, 2)]
        self.assertEqual(keys, expected_even_bounded)

        cursor.reset()
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        # Add odd keys inside and outside bounds.
        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_leader(odd_keys)
        self.do_checkpoint()

        cursor.set_key(self.fmt_key(500))
        self.assertEqual(cursor.search(), 0)
        cursor.reset()

        # Re-apply bounds after reset.
        cursor.set_key(self.fmt_key(200))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(800))
        cursor.bound("bound=upper")

        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        expected_all_bounded = [self.fmt_key(i) for i in range(200, 801)]
        self.assertEqual(keys, expected_all_bounded)
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: 1000 keys checkpointed. Follower deletes keys 400-599 locally.
    # Checkpoint 2 adds more keys. Locally deleted keys stay hidden; new keys appear.
    # -----------------------------------------------------------------------
    def test_checkpoint_advance_tombstone_persists(self):

        stable_keys = list(range(self.nkeys))
        self.insert_leader(stable_keys)
        self.do_checkpoint()

        delete_range = list(range(400, 600))
        self.remove_follower(delete_range)

        expected = [self.fmt_key(i) for i in range(self.nkeys) if i < 400 or i >= 600]
        self.assertEqual(self.scan_keys(self.session_follow), expected)

        # Advance the checkpoint with new leader keys; locally deleted keys must stay hidden.
        new_keys = list(range(self.nkeys, self.nkeys + 100))
        self.insert_leader(new_keys)
        self.do_checkpoint()

        expected_after = sorted(expected + [self.fmt_key(i) for i in new_keys])
        self.assertEqual(self.scan_keys(self.session_follow), expected_after)

    # -----------------------------------------------------------------------
    # Test: Leader sees its own writes immediately across checkpoints.
    # 500 keys, checkpoint, then add 500 more. Leader cursor sees all 1000.
    # -----------------------------------------------------------------------
    def test_leader_unaffected_by_checkpoint(self):

        first_half = list(range(0, 500))
        self.insert_leader(first_half)
        self.do_checkpoint()

        cursor = self.session.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(0))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(0))
        cursor.reset()

        second_half = list(range(500, self.nkeys))
        self.insert_leader(second_half)
        self.do_checkpoint()

        cursor.set_key(self.fmt_key(999))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(999))
        cursor.close()
