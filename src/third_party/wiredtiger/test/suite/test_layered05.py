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

# test_layered05.py
#
#   Exercises edge cases including:
#   - Non-exact searches where the nearest key is on either side.
#   - Deleted keys: search_near must skip tombstones and return a live neighbor.
#   - Correct iteration (next/prev) after search_near.

@disagg_test_class
class test_layered05(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    uri = 'layered:test_layered05'

    nkeys = 1000

    disagg_storages = gen_disagg_storages('test_layered05', disagg_only=True)

    scenarios = make_scenarios(disagg_storages)

    @staticmethod
    def fmt_key(i):
        return f"{i:06d}"

    @staticmethod
    def fmt_val(i):
        return f"val_{i:06d}"

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

    def next_ts(self):
        self.ts += 1
        return self.ts

    def insert_keys_on(self, session, keys, values=None):
        """Insert key/value pairs on a specific session."""
        cursor = session.open_cursor(self.uri)
        for i, key in enumerate(keys):
            val = values[i] if values else f"val_{key}"
            session.begin_transaction()
            cursor[key] = val
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def remove_keys_on(self, session, keys):
        """Remove keys on a specific session."""
        cursor = session.open_cursor(self.uri)
        for key in keys:
            session.begin_transaction()
            cursor.set_key(key)
            cursor.remove()
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def insert_stable(self, int_keys, values=None):
        """
        Write keys via the leader and checkpoint, making them visible to the follower.
        Accepts a list of integers; formats them internally.
        """
        keys = [self.fmt_key(i) for i in int_keys]
        vals = [self.fmt_val(i) for i in int_keys] if values is None else values
        self.insert_keys_on(self.session, keys, vals)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.ts)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    def insert_ingest(self, int_keys, values=None):
        """
        Write keys locally on the follower.
        Accepts a list of integers; formats them internally.
        """
        keys = [self.fmt_key(i) for i in int_keys]
        vals = [self.fmt_val(i) for i in int_keys] if values is None else values
        self.insert_keys_on(self.session_follow, keys, vals)

    def remove_ingest(self, int_keys):
        """
        Delete keys locally on the follower.
        Accepts a list of integers; formats them internally.
        """
        keys = [self.fmt_key(i) for i in int_keys]
        self.remove_keys_on(self.session_follow, keys)

    def _search_near_check_on(self, session, search_key, expected_key, expected_exact, expected_value=None):
        """Open a cursor on session, call search_near, and verify the result."""
        cursor = session.open_cursor(self.uri)
        cursor.set_key(search_key)
        exact = cursor.search_near()
        self.assertEqual(cursor.get_key(), expected_key,
            f"search_near({search_key}): expected key {expected_key}, got {cursor.get_key()}")
        self.assertEqual(exact, expected_exact,
            f"search_near({search_key}): expected exact={expected_exact}, got {exact}")
        if expected_value is not None:
            self.assertEqual(cursor.get_value(), expected_value,
                f"search_near({search_key}): expected value {expected_value}, got {cursor.get_value()}")
        cursor.close()

    def search_near_check(self, search_key, expected_key, expected_exact, expected_value=None):
        """Call search_near on the follower session and verify the result."""
        self._search_near_check_on(self.session_follow, search_key, expected_key, expected_exact, expected_value)

    def search_near_check_leader(self, search_key, expected_key, expected_exact, expected_value=None):
        """Call search_near on the leader session and verify the result."""
        self._search_near_check_on(self.session, search_key, expected_key, expected_exact, expected_value)

    def _search_near_notfound_on(self, session, search_key):
        """Verify search_near returns WT_NOTFOUND on the given session."""
        cursor = session.open_cursor(self.uri)
        cursor.set_key(search_key)
        ret = cursor.search_near()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND,
            f"search_near({search_key}): expected WT_NOTFOUND, got {ret}")
        cursor.close()

    def search_near_notfound(self, search_key):
        """Verify search_near returns WT_NOTFOUND on the follower session."""
        self._search_near_notfound_on(self.session_follow, search_key)

    def search_near_notfound_leader(self, search_key):
        """Verify search_near returns WT_NOTFOUND on the leader session."""
        self._search_near_notfound_on(self.session, search_key)

    def search_near_check_either(self, search_key, key_lower, key_upper):
        """
        Verify search_near on the follower returns either key_lower (cmp=-1) or
        key_upper (cmp=1). Used when both adjacent neighbors are valid results.
        """
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(search_key)
        exact = cursor.search_near()
        key = cursor.get_key()
        self.assertIn(key, [key_lower, key_upper],
            f"search_near({search_key}): expected {key_lower} or {key_upper}, got {key}")
        if key == key_lower:
            self.assertEqual(exact, -1,
                f"search_near({search_key}): key={key} (lower) but exact={exact}, expected -1")
        else:
            self.assertEqual(exact, 1,
                f"search_near({search_key}): key={key} (upper) but exact={exact}, expected 1")
        cursor.close()

    def assert_sorted_forward_from(self, search_key, n=5):
        """Position at search_key via search_near, then verify n forward steps are sorted."""
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(search_key)
        cursor.search_near()
        keys = [cursor.get_key()]
        for _ in range(n):
            self.assertEqual(cursor.next(), 0)
            keys.append(cursor.get_key())
        self.assertEqual(keys, sorted(keys))
        cursor.close()

    def assert_sorted_backward_from(self, search_key, n=5):
        """Position at search_key via search_near, then verify n backward steps are sorted in reverse."""
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(search_key)
        cursor.search_near()
        keys = [cursor.get_key()]
        for _ in range(n):
            self.assertEqual(cursor.prev(), 0)
            keys.append(cursor.get_key())
        self.assertEqual(keys, sorted(keys, reverse=True))
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Empty table returns WT_NOTFOUND.
    # -----------------------------------------------------------------------
    def test_search_near_empty(self):

        self.search_near_notfound("anything")

        # Also test after an empty checkpoint.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(1)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.search_near_notfound("anything")

    # -----------------------------------------------------------------------
    # Test: No checkpoint. Table has odd keys 1,3,...,999.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_only(self):

        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_ingest(odd_keys)

        # Exact match on an odd key.
        mid = 501
        self.search_near_check(self.fmt_key(mid), self.fmt_key(mid), 0, self.fmt_val(mid))

        # Key before all: only one valid answer.
        self.search_near_check(self.fmt_key(0), self.fmt_key(1), 1)

        # Key after all: only one valid answer.
        self.search_near_check(self.fmt_key(1000), self.fmt_key(999), -1)

        # Key between two odd keys: either neighbor is valid.
        self.search_near_check_either(self.fmt_key(500), self.fmt_key(499), self.fmt_key(501))
        self.search_near_check_either(self.fmt_key(100), self.fmt_key(99), self.fmt_key(101))

    # -----------------------------------------------------------------------
    # Test: All data checkpointed, no local writes.
    # For a non-exact search, either adjacent neighbor is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_stable_only(self):

        even_keys = list(range(0, self.nkeys, 2))
        self.insert_stable(even_keys)

        # Exact matches always work.
        self.search_near_check(self.fmt_key(500), self.fmt_key(500), 0, self.fmt_val(500))
        self.search_near_check(self.fmt_key(0), self.fmt_key(0), 0, self.fmt_val(0))
        self.search_near_check(self.fmt_key(998), self.fmt_key(998), 0, self.fmt_val(998))

        # Key before all: only one possible answer.
        self.search_near_check("", self.fmt_key(0), 1)
        # Key after all: only one possible answer.
        self.search_near_check(self.fmt_key(1000), self.fmt_key(998), -1)

        # Key between two existing keys: either neighbor is valid.
        self.search_near_check_either(self.fmt_key(501), self.fmt_key(500), self.fmt_key(502))

    # -----------------------------------------------------------------------
    # Test: Table has all keys 0-999 (even checkpointed, odd written locally).
    # search_near for a gap key should find the correct neighbor.
    # -----------------------------------------------------------------------
    def test_search_near_split_data(self):

        even_keys = list(range(0, self.nkeys, 2))
        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_stable(even_keys)
        self.insert_ingest(odd_keys)

        # Exact match on an odd key.
        self.search_near_check(self.fmt_key(501), self.fmt_key(501), 0, self.fmt_val(501))

        # Exact match on a stable key
        self.search_near_check(self.fmt_key(500), self.fmt_key(500), 0, self.fmt_val(500))

        # Key before all
        self.search_near_check("", self.fmt_key(0), 1)

        # Key after all
        self.search_near_check(self.fmt_key(1100), self.fmt_key(999), -1)

        # "000500x" sorts between 500 and 501: either neighbor is valid.
        self.search_near_check_either("000500x", self.fmt_key(500), self.fmt_key(501))

        # Verify forward and backward iteration from that position.
        self.assert_sorted_forward_from("000500x")
        self.assert_sorted_backward_from("000500x")

    # -----------------------------------------------------------------------
    # Test: Keys 0-499 are checkpointed; keys 500-999 are written locally.
    # Exact match and non-exact searches across the boundary.
    # -----------------------------------------------------------------------
    def test_search_near_opposite_sides(self):

        lower_keys = list(range(0, 500))
        upper_keys = list(range(500, self.nkeys))
        self.insert_stable(lower_keys)
        self.insert_ingest(upper_keys)

        # Exact match.
        self.search_near_check(self.fmt_key(500), self.fmt_key(500), 0)

        # Key between 499 and 500: either neighbor is valid.
        self.search_near_check_either("000499x", self.fmt_key(499), self.fmt_key(500))

        # Verify forward iteration from that position produces sorted keys.
        self.assert_sorted_forward_from("000499x")

    # -----------------------------------------------------------------------
    # Test: Table has keys 498 and 900. search_near(500).
    # Both adjacent neighbors are valid results: 498 (below) or 900 (above).
    # -----------------------------------------------------------------------
    def test_search_near_opposite_sides_farther(self):

        self.insert_stable([498])
        self.insert_ingest([900])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(498), self.fmt_key(900))

    # -----------------------------------------------------------------------
    # Test: Table has keys 200, 300, 900. search_near(500): adjacent neighbors are
    # 300 (below) and 900 (above); either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_neighbors_local_on_both_sides(self):

        self.insert_stable([200])
        self.insert_ingest([300, 900])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(300), self.fmt_key(900))

    # -----------------------------------------------------------------------
    # Test: Table has keys 100, 200, 900. search_near(500): adjacent neighbors are
    # 200 (below) and 900 (above); either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_neighbors_lower_from_checkpoint(self):

        self.insert_stable([200])
        self.insert_ingest([100, 900])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(200), self.fmt_key(900))

    # -----------------------------------------------------------------------
    # Test: Table has keys 200, 300, 600, 900. search_near(500): adjacent neighbors are
    # 300 (below) and 600 (above); either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_neighbors_nearest_upper_is_local(self):

        self.insert_stable([200])
        self.insert_ingest([300, 600, 900])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(300), self.fmt_key(600))

    # -----------------------------------------------------------------------
    # Test: Table has keys 100, 300, 800. search_near(500): adjacent neighbors are
    # 300 (below) and 800 (above); either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_neighbors_upper_from_checkpoint(self):

        self.insert_stable([800])
        self.insert_ingest([100, 300])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(300), self.fmt_key(800))

    # -----------------------------------------------------------------------
    # Test: Table has keys 100, 300, 600, 800. search_near(500): adjacent neighbors are
    # 300 (below) and 600 (above); either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_neighbors_nearest_lower_is_local(self):

        self.insert_stable([800])
        self.insert_ingest([100, 300, 600])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(300), self.fmt_key(600))

    # -----------------------------------------------------------------------
    # Test: Table has keys 600 and 800. search_near(500): both are above 500, returns nearest: 600.
    # -----------------------------------------------------------------------
    def test_search_near_both_larger(self):

        self.insert_stable([800])
        self.insert_ingest([600])

        self.search_near_check(self.fmt_key(500), self.fmt_key(600), 1)

    # -----------------------------------------------------------------------
    # Test: Table has keys 100 and 400. search_near(500): both are below 500, returns nearest: 400.
    # -----------------------------------------------------------------------
    def test_search_near_both_smaller(self):

        self.insert_stable([100])
        self.insert_ingest([400])

        self.search_near_check(self.fmt_key(500), self.fmt_key(400), -1)

    # -----------------------------------------------------------------------
    # Test: Stable: keys 200, 500, 700. Ingest: delete 500.
    # search_near(500): exact match is deleted; either neighbor 200 or 700 is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_exact_deleted(self):

        self.insert_stable([200, 500, 700])
        self.remove_ingest([500])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(200), self.fmt_key(700))

    # -----------------------------------------------------------------------
    # Test: Stable: keys 200, 500. Ingest: delete 500.
    # search_near(500): exact match is deleted; only smaller neighbor 200 exists.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_exact_deleted_walk_backward(self):

        self.insert_stable([200, 500])
        self.remove_ingest([500])

        self.search_near_check(self.fmt_key(500), self.fmt_key(200), -1)

    # -----------------------------------------------------------------------
    # Test: Table has keys 300 and 700; key 500 was inserted then deleted.
    # search_near(500): exact match is deleted; 300 and 700 are equidistant, either is valid.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_exact_deleted_stable_no_match(self):

        self.insert_stable([300, 700])
        self.insert_ingest([500])
        self.remove_ingest([500])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(300), self.fmt_key(700))

    # -----------------------------------------------------------------------
    # Test: Keys 300, 500, 700 were checkpointed then all deleted. Table is empty -> WT_NOTFOUND.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_exact_deleted_all_tombstoned(self):

        self.insert_stable([300, 500, 700])
        self.remove_ingest([300, 500, 700])

        self.search_near_notfound(self.fmt_key(500))

    # -----------------------------------------------------------------------
    # Test: All 1000 keys checkpointed then all deleted. Table is empty -> WT_NOTFOUND.
    # -----------------------------------------------------------------------
    def test_search_near_all_deleted(self):

        all_keys = list(range(0, self.nkeys))
        self.insert_stable(all_keys)
        self.remove_ingest(all_keys)

        self.search_near_notfound(self.fmt_key(500))

    # -----------------------------------------------------------------------
    # Test: Stable: keys 200, 700. Ingest: update 200, delete 700.
    # search_near(500): 700 is logically deleted; only 200 is live.
    # -----------------------------------------------------------------------
    def test_search_near_tombstone_cross_table(self):

        self.insert_stable([200, 700])
        self.insert_ingest([200])
        self.remove_ingest([700])

        self.search_near_check(self.fmt_key(500), self.fmt_key(200), -1)

    # -----------------------------------------------------------------------
    # Test: search_near followed by next/prev iteration produces correct order.
    # Stable: even keys 0,2,...,998. Ingest: odd keys 1,3,...,999.
    # -----------------------------------------------------------------------
    def test_search_near_then_iterate(self):

        even_keys = list(range(0, self.nkeys, 2))
        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_stable(even_keys)
        self.insert_ingest(odd_keys)

        session = self.session_follow
        cursor = session.open_cursor(self.uri)

        # search_near(500) -> exact match.
        cursor.set_key(self.fmt_key(500))
        exact = cursor.search_near()
        self.assertEqual(exact, 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(500))

        # Iterate forward: 501, 502, 503, ...
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(501))
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(502))
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(503))

        cursor.close()

        # Now test backward iteration from search_near
        cursor = session.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(500))
        exact = cursor.search_near()
        self.assertEqual(exact, 0)

        # Iterate backward: 499, 498, 497
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(499))
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(498))
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(497))

        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Stable: all keys 0-999. Ingest: delete 500.
    # search_near(500): exact match is deleted; 499 and 501 are equidistant,
    # either is a valid result. Iterating past the returned key must skip the
    # deleted 500 and land on the other neighbor.
    # -----------------------------------------------------------------------
    def test_search_near_tombstone_then_iterate(self):

        all_keys = list(range(0, self.nkeys))
        self.insert_stable(all_keys)
        self.remove_ingest([500])

        session = self.session_follow
        cursor = session.open_cursor(self.uri)

        cursor.set_key(self.fmt_key(500))
        exact = cursor.search_near()
        self.assertNotEqual(exact, 0)
        landed = cursor.get_key()
        self.assertIn(landed, [self.fmt_key(499), self.fmt_key(501)])

        if landed == self.fmt_key(501):
            # Returned key is above 500; iterating backward must skip 500 and land on 499.
            self.assertEqual(cursor.prev(), 0)
            self.assertEqual(cursor.get_key(), self.fmt_key(499))
        else:
            # Returned key is below 500; iterating forward must skip 500 and land on 501.
            self.assertEqual(cursor.next(), 0)
            self.assertEqual(cursor.get_key(), self.fmt_key(501))

        cursor.close()

    # -----------------------------------------------------------------------
    # Test: Multiple tombstones in a row.
    # Stable: all keys 0-999. Ingest: tombstones for 400-600.
    # All three search keys fall in the deleted range [400,600]; live neighbors
    # are 399 (below) and 601 (above). For 400 and 500 either neighbor is valid;
    # for 600 the nearest live key is clearly 601.
    # After search_near(500), iterating forward must skip all deleted keys and
    # produce exactly keys 601-999 in order.
    # -----------------------------------------------------------------------
    def test_search_near_consecutive_tombstones(self):

        all_keys = list(range(0, self.nkeys))
        tombstoned = list(range(400, 601))
        self.insert_stable(all_keys)
        self.remove_ingest(tombstoned)

        self.search_near_check_either(self.fmt_key(400), self.fmt_key(399), self.fmt_key(601))
        self.search_near_check_either(self.fmt_key(500), self.fmt_key(399), self.fmt_key(601))
        self.search_near_check(self.fmt_key(600), self.fmt_key(601), 1)

        # Verify forward iteration from search_near(500) skips all deleted keys.
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(500))
        self.assertNotEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        first_key = cursor.get_key()

        keys = [first_key]
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        cursor.close()

        deleted = set(self.fmt_key(k) for k in tombstoned)
        for k in keys:
            self.assertNotIn(k, deleted, f"Deleted key appeared: {k}")

        # All keys from first live key above 600 onward must be present.
        expected_tail = [self.fmt_key(k) for k in range(601, self.nkeys)]
        self.assertEqual(keys[-len(expected_tail):], expected_tail)

    # -----------------------------------------------------------------------
    # Test: Verify full forward and backward scan with interleaved data.
    # Stable: even keys 0,2,...,998. Ingest: odd keys 1,3,...,999.
    # Full scan should see all 1000 keys in order.
    # -----------------------------------------------------------------------
    def test_search_near_full_scan_interleaved(self):

        even_keys = list(range(0, self.nkeys, 2))
        odd_keys = list(range(1, self.nkeys, 2))
        self.insert_stable(even_keys)
        self.insert_ingest(odd_keys)

        session = self.session_follow
        expected = [self.fmt_key(i) for i in range(self.nkeys)]

        # Position at the start via search_near
        cursor = session.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(0))
        exact = cursor.search_near()
        self.assertEqual(exact, 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(0))

        # Walk forward
        keys = [self.fmt_key(0)]
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, expected)
        cursor.close()

        # Walk backward from end
        cursor = session.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(999))
        exact = cursor.search_near()
        self.assertEqual(exact, 0)

        keys = [self.fmt_key(999)]
        while cursor.prev() == 0:
            keys.append(cursor.get_key())
        self.assertEqual(keys, list(reversed(expected)))
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: 1000 keys checkpointed with old values, then all overwritten locally with new values.
    # search_near returns the new value (local writes take precedence over checkpointed data).
    # -----------------------------------------------------------------------
    def test_search_near_ingest_overrides_stable(self):

        all_keys = list(range(0, self.nkeys))
        self.insert_stable(all_keys)
        self.insert_ingest(all_keys)

        self.search_near_check(self.fmt_key(500), self.fmt_key(500), 0)

    # -----------------------------------------------------------------------
    # Test: Table has keys 0-499 and 700. search_near(1100): key is beyond all keys, returns 700.
    # -----------------------------------------------------------------------
    def test_search_near_beyond_max(self):

        lower_keys = list(range(0, 500))
        self.insert_stable(lower_keys)
        self.insert_ingest([700])

        self.search_near_check(self.fmt_key(1100), self.fmt_key(700), -1)

    # -----------------------------------------------------------------------
    # Test: No checkpoint. Table has keys 100, 700; key 500 is deleted.
    # search_near(500): exact match is deleted; live neighbors are 100 and 700,
    # either is a valid result.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_tombstone_no_stable_forward(self):

        self.insert_ingest([100, 500, 700])
        self.remove_ingest([500])

        self.search_near_check_either(self.fmt_key(500), self.fmt_key(100), self.fmt_key(700))

    # -----------------------------------------------------------------------
    # Test: No checkpoint. Table has key 100; key 500 is deleted.
    # search_near(500): exact match is deleted; only smaller neighbor 100 exists.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_tombstone_no_stable_backward(self):

        self.insert_ingest([100, 500])
        self.remove_ingest([500])

        self.search_near_check(self.fmt_key(500), self.fmt_key(100), -1)

    # -----------------------------------------------------------------------
    # Test: No checkpoint. Only key 500 was inserted then deleted. Table is empty -> WT_NOTFOUND.
    # -----------------------------------------------------------------------
    def test_search_near_ingest_tombstone_no_stable_notfound(self):

        self.insert_ingest([500])
        self.remove_ingest([500])

        self.search_near_notfound(self.fmt_key(500))

    # -----------------------------------------------------------------------
    # Test: All 1000 keys in stable. Follower deletes keys 500-999 (upper half).
    # search_near(700) must land on a key below 500; prev() from there must
    # return all remaining keys in reverse order with no deleted key appearing.
    # -----------------------------------------------------------------------
    def test_search_near_tombstone_walk_then_prev(self):
        """
        search_near on a deleted key where all keys from the search key forward
        are also deleted. The nearest live key is below the search key.
        A subsequent prev() scan must continue in reverse order without returning
        deleted keys.
        """

        self.insert_stable(list(range(self.nkeys)))
        self.remove_ingest(list(range(500, self.nkeys)))

        cursor = self.session_follow.open_cursor(self.uri)

        # search_near(700): key and all keys above 500 are deleted; nearest live key is 499 (below).
        cursor.set_key(self.fmt_key(700))
        exact = cursor.search_near()
        self.assertNotEqual(exact, wiredtiger.WT_NOTFOUND)
        first_key = cursor.get_key()
        self.assertLess(first_key, self.fmt_key(500),
            f"Expected key < 000500, got {first_key}")

        # Iterate backward from the result.
        keys = [first_key]
        while cursor.prev() == 0:
            keys.append(cursor.get_key())

        for i in range(len(keys) - 1):
            self.assertGreater(keys[i], keys[i + 1],
                f"Out of order at {i}: {keys[i]} <= {keys[i + 1]}")
        cursor.close()

    # -----------------------------------------------------------------------
    # Test: All 1000 keys in stable. Follower deletes range 300-600.
    # Bounded cursor [200, 800]. search_near(450): key is deleted; either
    # neighbor 299 or 601 is a valid result. Iterating forward must stay
    # within bounds with no deleted key appearing.
    # -----------------------------------------------------------------------
    def test_search_near_tombstone_walk_then_next_with_bounds(self):
        """
        Bounded search_near + tombstone + next. This is the MongoDB
        pattern: set bounds, search_near to position, iterate.
        """

        self.insert_stable(list(range(self.nkeys)))

        # Tombstone a range that overlaps the search key.
        self.remove_ingest(list(range(300, 601)))

        lo, hi = 200, 800
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(self.fmt_key(lo))
        cursor.bound("bound=lower")
        cursor.set_key(self.fmt_key(hi))
        cursor.bound("bound=upper")

        # search_near(450): key is deleted; either neighbor 299 or 601 is a valid result.
        cursor.set_key(self.fmt_key(450))
        exact = cursor.search_near()
        self.assertNotEqual(exact, wiredtiger.WT_NOTFOUND)
        first_key = cursor.get_key()

        keys = [first_key]
        while cursor.next() == 0:
            keys.append(cursor.get_key())

        for i in range(len(keys) - 1):
            self.assertLess(keys[i], keys[i + 1],
                f"Out of order at {i}: {keys[i]} >= {keys[i + 1]}")

        # All within bounds.
        for k in keys:
            self.assertGreaterEqual(k, self.fmt_key(lo))
            self.assertLessEqual(k, self.fmt_key(hi))

        # No tombstoned keys.
        tombstoned = set(self.fmt_key(k) for k in range(300, 601))
        for k in keys:
            self.assertNotIn(k, tombstoned)
        cursor.close()
