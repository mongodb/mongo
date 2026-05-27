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

# test_layered_fast_truncate12.py
#   Cursor iteration and searches over truncated ranges.
#
#   Verify that forward scans, backward scans, next_random, search, and
#   search_near all treat truncated keys as non-existent on a follower.

from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import (
    LayeredFastTruncateConfigMixin, concat, range_inclusive,
)
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate12(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    """
    Cursor iteration and searches over truncated ranges.

    Verify that forward scans, backward scans, next_random, search, and
    search_near all treat truncated keys as non-existent on a follower.
    """

    uris = [
        ("layered", {"uri": "layered:fast_truncate"}),
        ("table", {"uri": "table:fast_truncate"}),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)
    conn_config = 'disaggregated=(role="leader"),'

    def random_sample_keys(self, n):
        """Return n keys drawn from a next_random cursor."""
        result = []
        with self.auto_closing_cursor("next_random=true") as cursor:
            with self.transaction(rollback=True):
                for _ in range(n):
                    cursor.next()
                    result.append(cursor.get_key())
        return result

    def test_forward_scan_skips_truncated_range(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # Forward iteration should yield keys 1-29 and 61-100 in order.
        expected = concat(range_inclusive(1, 29), range_inclusive(61, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_backward_scan_skips_truncated_range(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # Backward iteration should yield keys 100-61 and 29-1 in order.
        expected = concat(
            reversed(range_inclusive(61, 100)),
            reversed(range_inclusive(1, 29)),
        )
        self.assertEqual(self.visible_keys(forward=False), expected)

    def test_next_random_never_lands_in_truncated_range(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # No sample from 200 random draws should fall inside the truncated
        # range.
        samples = self.random_sample_keys(200)
        self.assertFalse(any(30 <= k <= 60 for k in samples))

    def test_search_inside_truncated_range(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # Searching for a key inside the truncated range should return
        # WT_NOTFOUND.
        self.assertFalse(self.key_exists(45))

    def test_search_at_inclusive_truncate_boundary(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # The boundary keys should be invisible.
        self.assertFalse(self.key_exists(30))
        self.assertFalse(self.key_exists(60))

        # The keys just outside the truncated range should still be found.
        self.assertTrue(self.key_exists(29))
        self.assertTrue(self.key_exists(61))

    def test_search_near_inside_truncated_range(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # search_near(45) scans forward through the truncated range and exits
        # at 61 (exact=1).
        exact, found_key = self.search_near_key(45)
        self.assertEqual(exact, 1)
        self.assertEqual(found_key, 61)

    def test_search_near_at_inclusive_truncate_boundary(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60.
        self.truncate(30, 60)

        # search_near scans forward from either boundary, traverses the
        # truncated range, and exits at 61 in both cases.
        _, found_key_30 = self.search_near_key(30)
        _, found_key_60 = self.search_near_key(60)
        self.assertEqual(found_key_30, 61)
        self.assertEqual(found_key_60, 61)

    def test_search_near_prefers_forward_then_falls_back_to_backward(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate from key 30 to the end of the table, leaving only keys 1-29
        # visible.
        self.truncate(30, None)

        # search_near(45) scans forward but the truncated range extends to the
        # end of the table; falls back to 29 (exact=-1).
        exact, found_key = self.search_near_key(45)
        self.assertEqual(exact, -1)
        self.assertEqual(found_key, 29)

    def test_search_near_picks_closer_live_ingest_over_advanced_stable(self):
        # Set up a follower with stable keys 1-100, then truncate keys 40-70.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()
        self.truncate(40, 70)

        # Write key 65 into the truncated range after the truncate has
        # committed.
        self.populate([65])

        # search_near(60) scans forward and reaches live-ingest key 65 before
        # stable key 71 (the first key past the original truncated range).
        exact, found_key = self.search_near_key(60)
        self.assertEqual(exact, 1)
        self.assertEqual(found_key, 65)


if __name__ == "__main__":
    wttest.run()
