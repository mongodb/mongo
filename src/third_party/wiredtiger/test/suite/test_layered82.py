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

# test_layered82.py
#   Test cursor bounds on layered cursors with a 1000-key dataset.

@disagg_test_class
class test_layered82(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    uri = 'layered:test_layered82'
    nkeys = 1000

    disagg_storages = gen_disagg_storages('test_layered82', disagg_only=True)

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

    def next_ts(self):
        self.ts += 1
        return self.ts

    @staticmethod
    def fmt_key(i):
        return f"{i:06d}"

    @staticmethod
    def fmt_val(i):
        return f"val_{i:06d}"

    # -----------------------------------------------------------------------
    # Low-level insert/remove helpers (accept lists of ints).
    # -----------------------------------------------------------------------

    def insert_stable(self, keys, values=None):
        """Insert keys (list of ints) into stable via leader checkpoint."""
        cursor = self.session.open_cursor(self.uri)
        for idx, i in enumerate(keys):
            key = self.fmt_key(i)
            val = values[idx] if values else self.fmt_val(i)
            self.session.begin_transaction()
            cursor[key] = val
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.ts)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    def insert_ingest(self, keys, values=None):
        """Insert keys (list of ints) into the follower's local table."""
        session = self.session_follow
        cursor = session.open_cursor(self.uri)
        for idx, i in enumerate(keys):
            key = self.fmt_key(i)
            val = values[idx] if values else self.fmt_val(i)
            session.begin_transaction()
            cursor[key] = val
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    def remove_ingest(self, keys):
        """Remove keys (list of ints) from the follower's local table."""
        session = self.session_follow
        cursor = session.open_cursor(self.uri)
        for i in keys:
            key = self.fmt_key(i)
            session.begin_transaction()
            cursor.set_key(key)
            cursor.remove()
            session.commit_transaction(f"commit_timestamp={self.timestamp_str(self.next_ts())}")
        cursor.close()

    # -----------------------------------------------------------------------
    # Data population helpers  each creates a common data layout.
    # -----------------------------------------------------------------------

    def populate_interleaved(self):
        """Checkpointed even keys and locally-written odd keys. Returns all expected keys."""
        self.insert_stable(list(range(0, self.nkeys, 2)))
        self.insert_ingest(list(range(1, self.nkeys, 2)))
        return [self.fmt_key(i) for i in range(self.nkeys)]

    def populate_all_stable(self):
        """All keys checkpointed. Returns all expected keys."""
        self.insert_stable(list(range(self.nkeys)))
        return [self.fmt_key(i) for i in range(self.nkeys)]

    def populate_all_ingest(self):
        """All keys written locally (no checkpoint). Returns all expected keys."""
        self.insert_ingest(list(range(self.nkeys)))
        return [self.fmt_key(i) for i in range(self.nkeys)]

    # -----------------------------------------------------------------------
    # Cursor helpers.
    # -----------------------------------------------------------------------

    def set_bounds(self, cursor, lower=None, upper=None,
                   lower_inclusive=True, upper_inclusive=True):
        """Set bounds on a cursor. Pass None to skip a bound."""
        if lower is not None:
            cursor.set_key(lower)
            incl = "true" if lower_inclusive else "false"
            cursor.bound(f"bound=lower,inclusive={incl}")
        if upper is not None:
            cursor.set_key(upper)
            incl = "true" if upper_inclusive else "false"
            cursor.bound(f"bound=upper,inclusive={incl}")

    def scan_forward(self, cursor):
        """Scan forward and return list of keys."""
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        return keys

    def scan_backward(self, cursor):
        """Scan backward and return list of keys."""
        keys = []
        while cursor.prev() == 0:
            keys.append(cursor.get_key())
        return keys

    def open_bounded_cursor(self, lower=None, upper=None,
                            lower_inclusive=True, upper_inclusive=True):
        """Open a cursor on the session under test and set bounds."""
        cursor = self.session_follow.open_cursor(self.uri)
        self.set_bounds(cursor, lower, upper, lower_inclusive, upper_inclusive)
        return cursor

    def expected_range(self, lo, hi):
        """Return list of formatted keys for range [lo, hi] inclusive."""
        return [self.fmt_key(i) for i in range(lo, hi + 1)]

    # =====================================================================
    # Tests
    # =====================================================================

    def test_bounds_ingest_only(self):
        """Bounds with all data written locally (no checkpoint)."""
        self.populate_all_ingest()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))
        cursor.close()

    def test_bounds_stable_only(self):
        """Bounds with all data checkpointed."""
        self.populate_all_stable()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))
        cursor.close()

    def test_bounds_split_data(self):
        """Bounds with interleaved data (checkpointed even keys, locally-written odd keys)."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))
        cursor.close()

    def test_bounds_split_data_prev(self):
        """Bounds with interleaved data, reverse iteration."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        expected = [self.fmt_key(i) for i in range(800, 199, -1)]
        self.assertEqual(self.scan_backward(cursor), expected)
        cursor.close()

    def test_bounds_lower_only(self):
        """Lower bound only at 500. Expected: 500-999."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(500))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(500, 999))
        cursor.close()

    def test_bounds_upper_only(self):
        """Upper bound only at 500. Expected: 0-500."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(upper=self.fmt_key(500))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(0, 500))
        cursor.close()

    def test_bounds_exclusive_lower(self):
        """Exclusive lower bound at 200. Expected: 201-999."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), lower_inclusive=False)
        self.assertEqual(self.scan_forward(cursor), self.expected_range(201, 999))
        cursor.close()

    def test_bounds_exclusive_upper(self):
        """Exclusive upper bound at 800. Expected: 0-799."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(upper=self.fmt_key(800), upper_inclusive=False)
        self.assertEqual(self.scan_forward(cursor), self.expected_range(0, 799))
        cursor.close()

    def test_bounds_both_exclusive(self):
        """Both bounds exclusive: (200, 800). Expected: 201-799."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(
            lower=self.fmt_key(200), upper=self.fmt_key(800),
            lower_inclusive=False, upper_inclusive=False)
        self.assertEqual(self.scan_forward(cursor), self.expected_range(201, 799))
        cursor.close()

    def test_bounds_nonexistent_keys(self):
        """Bounds at keys that don't exist. Even keys only, bounds [201, 799]."""
        self.insert_stable(list(range(0, self.nkeys, 2)))

        cursor = self.open_bounded_cursor(lower=self.fmt_key(201), upper=self.fmt_key(799))
        expected = [self.fmt_key(i) for i in range(202, 800, 2)]
        self.assertEqual(self.scan_forward(cursor), expected)
        cursor.close()

    def test_bounds_no_data_in_range(self):
        """Bounds [1500, 2000] beyond all data. Expected: empty."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(1500), upper=self.fmt_key(2000))
        self.assertEqual(self.scan_forward(cursor), [])
        cursor.close()

    def test_bounds_tombstone_inside(self):
        """Tombstone every 3rd key in [200, 800]. Bounds [200, 800]."""
        self.populate_all_stable()
        self.remove_ingest([i for i in range(200, 801) if i % 3 == 0])

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        expected = [self.fmt_key(i) for i in range(200, 801) if i % 3 != 0]
        self.assertEqual(self.scan_forward(cursor), expected)
        cursor.close()

    def test_bounds_tombstone_at_bounds(self):
        """Tombstone the bound keys 200 and 800. Bounds [200, 800]. Expected: 201-799."""
        self.populate_all_stable()
        self.remove_ingest([200, 800])

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(201, 799))
        cursor.close()

    def test_bounds_all_tombstoned_in_range(self):
        """Tombstone everything in [200, 800]. Bounds [200, 800]. Expected: empty."""
        self.populate_all_stable()
        self.remove_ingest(list(range(200, 801)))

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), [])
        cursor.close()

    def test_bounds_tombstone_outside(self):
        """Tombstone keys outside [200, 800]. Bounds [200, 800]. Range intact."""
        self.populate_all_stable()
        self.remove_ingest(list(range(0, 200)) + list(range(801, self.nkeys)))

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))
        cursor.close()

    def test_bounds_clear(self):
        """Set bounds, scan, clear bounds, rescan sees all keys."""
        all_keys = self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))

        cursor.reset()
        cursor.bound("action=clear")

        self.assertEqual(self.scan_forward(cursor), all_keys)
        cursor.close()

    def test_bounds_search_near(self):
        """search_near below bounds [300, 700]: nearest key within bounds is 300 (cmp=1)."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(300), upper=self.fmt_key(700))
        cursor.set_key(self.fmt_key(100))
        exact = cursor.search_near()
        self.assertEqual(cursor.get_key(), self.fmt_key(300))
        self.assertEqual(exact, 1)
        cursor.close()

    def test_bounds_search_near_upper(self):
        """search_near above bounds [300, 700]: nearest key within bounds is 700 (cmp=-1)."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(300), upper=self.fmt_key(700))
        cursor.set_key(self.fmt_key(900))
        exact = cursor.search_near()
        self.assertEqual(cursor.get_key(), self.fmt_key(700))
        self.assertEqual(exact, -1)
        cursor.close()

    def test_bounds_set_before_data(self):
        """Bounds set before any data is inserted."""

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), [])
        cursor.close()

        self.insert_ingest(list(range(self.nkeys)))
        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(200, 800))
        cursor.close()

    def test_bounds_ingest_overrides_stable(self):
        """Local write overrides checkpointed value within bounds."""
        self.populate_all_stable()
        self.insert_ingest([500], values=["new_500"])

        cursor = self.open_bounded_cursor(lower=self.fmt_key(500), upper=self.fmt_key(500))
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), self.fmt_key(500))
        self.assertEqual(cursor.get_value(), "new_500")
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_bounds_adjacent_exclusive(self):
        """Exclusive bounds (199, 201). Expected: only key 200."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(
            lower=self.fmt_key(199), upper=self.fmt_key(201),
            lower_inclusive=False, upper_inclusive=False)
        self.assertEqual(self.scan_forward(cursor), [self.fmt_key(200)])
        cursor.close()

    def test_bounds_single_point(self):
        """Single-point bounds [500, 500]. Expected: only key 500."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(500), upper=self.fmt_key(500))
        self.assertEqual(self.scan_forward(cursor), [self.fmt_key(500)])

        # Also test prev.
        cursor.reset()
        self.set_bounds(cursor, lower=self.fmt_key(500), upper=self.fmt_key(500))
        self.assertEqual(self.scan_backward(cursor), [self.fmt_key(500)])
        cursor.close()

    def test_bounds_search(self):
        """search inside bounds succeeds, outside bounds fails."""
        self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(200), upper=self.fmt_key(800))

        cursor.set_key(self.fmt_key(500))
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), self.fmt_val(500))

        cursor.set_key(self.fmt_key(100))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        cursor.set_key(self.fmt_key(900))
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_bounds_rebind(self):
        """Rebind to narrower bounds without clearing first."""
        all_keys = self.populate_interleaved()

        cursor = self.open_bounded_cursor(lower=self.fmt_key(0), upper=self.fmt_key(999))
        self.assertEqual(self.scan_forward(cursor), all_keys)

        # Reset the bounds.
        cursor.reset()

        self.set_bounds(cursor, lower=self.fmt_key(400), upper=self.fmt_key(600))
        self.assertEqual(self.scan_forward(cursor), self.expected_range(400, 600))
        cursor.close()

    def test_bounds_positioned_update_mid_scan(self):
        """
        Bounded scan, positioned update mid-scan, continue scanning. Verifies
        that a write while the cursor is positioned within a bounded scan does
        not disrupt iteration order, and that all returned keys stay within bounds.
        """

        # Interleaved: even keys checkpointed, odd keys written locally.
        self.insert_stable(list(range(0, self.nkeys, 2)))
        self.insert_ingest(list(range(1, self.nkeys, 2)))

        lo, hi = 200, 800
        cursor = self.session_follow.open_cursor(self.uri)
        self.set_bounds(cursor, lower=self.fmt_key(lo), upper=self.fmt_key(hi))

        keys_before = []
        for _ in range(100):
            self.assertEqual(cursor.next(), 0)
            keys_before.append(cursor.get_key())

        # Positioned update at the current cursor position mid-scan.
        self.session_follow.begin_transaction()
        cursor.set_value("updated")
        cursor.update()
        self.session_follow.commit_transaction(
            f"commit_timestamp={self.timestamp_str(self.next_ts())}")

        keys_after = []
        while cursor.next() == 0:
            keys_after.append(cursor.get_key())

        all_keys = keys_before + keys_after
        for i in range(len(all_keys) - 1):
            self.assertLess(all_keys[i], all_keys[i + 1],
                f"Out of order at {i}: {all_keys[i]} >= {all_keys[i + 1]}")

        # All keys must stay within the declared bounds.
        for k in all_keys:
            self.assertGreaterEqual(k, self.fmt_key(lo))
            self.assertLessEqual(k, self.fmt_key(hi))
        cursor.close()
