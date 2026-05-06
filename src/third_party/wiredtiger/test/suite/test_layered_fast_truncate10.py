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

# test_layered_fast_truncate10.py
#   Data location semantics (stable vs ingest).
#
#   Verify that fast-truncate on a follower behaves as a single operation over
#   the logical union of the stable and ingest tables, independent of which
#   table any given key actually lives in.

from contextlib import closing
from itertools import chain
from typing import Iterable
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import disagg_fast_truncate_build
from wtscenario import make_scenarios
import wttest


def concat(*iterables: Iterable[int]) -> list[int]:
    """Concatenate any number of iterables into a single list."""
    return list(chain.from_iterable(iterables))


def range_inclusive(start: int, stop: int) -> range:
    """Return a range covering [start, stop] inclusive."""
    return range(start, stop + 1)


@disagg_test_class
class test_layered_fast_truncate10(wttest.WiredTigerTestCase):
    """
    Data location semantics (stable vs ingest).

    Verify that fast-truncate on a follower behaves as a single operation over
    the logical union of the stable and ingest tables, independent of which
    table any given key actually lives in.
    """

    # WiredTiger config:
    uris = [
        ("layered", {"uri": "layered:fast_truncate"}),
        ("table", {"uri": "table:fast_truncate"}),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)
    conn_config = 'disaggregated=(role="leader"),'

    def setUp(self):
        if disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    def auto_closing_cursor(self):
        """Return a cursor that auto-closes as it goes out of scope."""
        return closing(self.session.open_cursor(self.uri))

    def populate(self, keys: Iterable[int]):
        """Insert each key with a placeholder value in a single transaction."""
        with self.auto_closing_cursor() as cursor:
            with self.transaction():
                for key in keys:
                    cursor[key] = "v"

    def setup_leader(self, keys: Iterable[int] | None = None):
        """
        Create the table on the leader and optionally pre-populate stable.
        The follower will pick up these keys via the initial checkpoint.
        """
        self.session.create(self.uri, "key_format=i,value_format=S")
        if keys is not None:
            self.populate(keys)
        self.session.checkpoint()

    def setup_follower(self, keys: Iterable[int] | None = None):
        """Switch to follower role and optionally write keys to ingest."""
        self.reopen_disagg_conn('disaggregated=(role="follower"),')
        if keys is not None:
            self.populate(keys)

    def truncate(self, start_key: int, stop_key: int):
        """Truncate between start and stop keys inclusive."""
        with (
            self.auto_closing_cursor() as start_cursor,
            self.auto_closing_cursor() as stop_cursor,
        ):
            start_cursor.set_key(start_key)
            stop_cursor.set_key(stop_key)

            with self.transaction():
                self.session.truncate(None, start_cursor, stop_cursor, None)

    def visible_keys(self) -> list[int]:
        """Return all keys visible via a forward scan, in key order."""
        result = []
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                while cursor.next() == 0:
                    result.append(cursor.get_key())
        return result

    def test_truncate_range_with_both_tables_empty(self):
        # Stable and ingest are both empty.
        self.setup_leader()
        self.setup_follower()

        # Truncate an arbitrary key range on the empty follower.
        self.truncate(50, 60)

        # No keys exist anywhere, so the table should still appear empty.
        self.assertEqual(self.visible_keys(), [])

    def test_truncate_range_hits_no_keys_in_either_table(self):
        # Some keys in stable and some in ingest.
        self.setup_leader(keys=range_inclusive(10, 20))
        self.setup_follower(keys=range_inclusive(30, 40))

        # Truncate a range that does not overlap either table's keys.
        self.truncate(50, 60)

        # All original keys should remain visible since none were in range.
        expected = concat(range_inclusive(10, 20), range_inclusive(30, 40))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_covers_stable_keys_ingest_empty(self):
        # Stable keys only.
        self.setup_leader(keys=range_inclusive(10, 50))
        self.setup_follower()

        # Truncate a range that covers a slice of stable keys.
        self.truncate(20, 40)

        # Only stable keys outside the truncated range should remain.
        expected = concat(range_inclusive(10, 19), range_inclusive(41, 50))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_covers_stable_keys_ingest_disjoint(self):
        # Stable keys and ingest keys in a disjoint range.
        self.setup_leader(keys=range_inclusive(10, 50))
        self.setup_follower(keys=range_inclusive(70, 90))

        # Truncate a range that covers a slice of stable keys only.
        self.truncate(20, 40)

        # Stable keys outside the range remain; ingest keys are untouched.
        expected = concat(
            range_inclusive(10, 19),
            range_inclusive(41, 50),
            range_inclusive(70, 90),
        )
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_covers_ingest_keys_stable_empty(self):
        # Ingest keys only.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(30, 70))

        # Truncate a range that covers a slice of ingest keys.
        self.truncate(40, 60)

        # Only ingest keys outside the truncated range should remain.
        expected = concat(range_inclusive(30, 39), range_inclusive(61, 70))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_covers_ingest_keys_stable_disjoint(self):
        # Stable keys and ingest keys in disjoint ranges.
        self.setup_leader(keys=range_inclusive(10, 20))
        self.setup_follower(keys=range_inclusive(30, 70))

        # Truncate a range that covers a slice of ingest keys only.
        self.truncate(40, 60)

        # Ingest keys outside the range remain; stable keys are untouched.
        expected = concat(
            range_inclusive(10, 20),
            range_inclusive(30, 39),
            range_inclusive(61, 70),
        )
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_covers_keys_in_both_tables(self):
        # Stable and ingest hold overlapping ranges.
        self.setup_leader(keys=range_inclusive(10, 50))
        self.setup_follower(keys=range_inclusive(30, 70))

        # Truncate a range that covers keys present in both tables.
        self.truncate(20, 60)

        # Only keys outside the truncated range should remain visible,
        # regardless of which table they came from.
        expected = concat(range_inclusive(10, 19), range_inclusive(61, 70))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_range_partially_overlaps_both_tables(self):
        # Stable and ingest hold partially overlapping ranges.
        self.setup_leader(keys=range_inclusive(10, 50))
        self.setup_follower(keys=range_inclusive(30, 70))

        # Truncate a range that spans part of stable and part of ingest.
        self.truncate(40, 60)

        # Only the keys outside the truncated range should still be visible.
        expected = concat(range_inclusive(10, 39), range_inclusive(61, 70))
        self.assertEqual(self.visible_keys(), expected)


if __name__ == "__main__":
    wttest.run()
