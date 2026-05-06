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

# test_layered_fast_truncate13.py
#   Interactions with existing truncates.
#
#   Verify that subsequent operations - additional truncates, per-key removes,
#   and reinsertion - compose correctly with a prior committed truncate.

from contextlib import closing, nullcontext
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
class test_layered_fast_truncate13(wttest.WiredTigerTestCase):
    """
    Interactions with existing truncates.

    Verify that subsequent operations - additional truncates, per-key removes,
    and reinsertion - compose correctly with a prior committed truncate.
    """

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

    def session_create_config(self):
        cfg = "key_format=i,value_format=S"
        if self.uri.startswith("table"):
            cfg += ",block_manager=disagg,type=layered"
        return cfg

    def auto_closing_cursor(self, config: str | None = None) -> closing:
        """Return a cursor that auto-closes as it goes out of scope."""
        return closing(self.session.open_cursor(self.uri, None, config))

    def populate(self, keys: Iterable[int]):
        """Insert each key with a placeholder value in a single transaction."""
        with self.auto_closing_cursor() as cursor:
            with self.transaction():
                for key in keys:
                    cursor[key] = "v"

    def setup_leader(self, keys: Iterable[int] | None = None):
        """
        Create the table on the leader and optionally pre-populate stable. The
        follower will pick up these keys via the initial checkpoint.
        """
        self.session.create(self.uri, self.session_create_config())
        if keys is not None:
            self.populate(keys)
        self.session.checkpoint()

    def setup_follower(self, keys: Iterable[int] | None = None):
        """Switch to follower role and optionally write keys to ingest."""
        self.reopen_disagg_conn('disaggregated=(role="follower"),')
        if keys is not None:
            self.populate(keys)

    def cursor_for_key(self, key: int | None):
        """Return a cursor with its key set, or None if key is None."""
        if key is None:
            return nullcontext(None)
        cursor = self.auto_closing_cursor()
        cursor.thing.set_key(key)
        return cursor

    def truncate(self, start_key: int | None, stop_key: int | None):
        """Truncate [start_key, stop_key] inclusive; None means open end."""
        with (
            self.cursor_for_key(start_key) as start,
            self.cursor_for_key(stop_key) as stop,
        ):
            uri = self.uri if (start is None and stop is None) else None
            with self.transaction():
                self.session.truncate(uri, start, stop, None)

    def remove_key(self, key: int):
        """Remove a single key in a transaction."""
        with self.cursor_for_key(key) as cursor:
            with self.transaction():
                cursor.remove()

    def visible_keys(self) -> list[int]:
        """Return all keys visible via a forward scan, in key order."""
        result = []
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                while cursor.next() == 0:
                    result.append(cursor.get_key())
        return result

    def test_per_key_removes_before_truncate(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Remove key 45, then truncate keys 30-60.
        self.remove_key(45)
        self.truncate(30, 60)

        # Keys 30-60 are invisible; keys outside remain visible.
        expected = concat(range_inclusive(1, 29), range_inclusive(61, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_same_range_twice(self):
        # Set up a follower with keys 1-100, then truncate keys 30-60.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))
        self.truncate(30, 60)

        # Truncate the same range again in a new transaction.
        self.truncate(30, 60)

        # Keys 30-60 remain invisible; no error or corruption.
        expected = concat(range_inclusive(1, 29), range_inclusive(61, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_already_truncated_superset_range(self):
        # Set up a follower with keys 1-100, then truncate keys 40-50.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))
        self.truncate(40, 50)

        # Truncate the broader range 30-60.
        self.truncate(30, 60)

        # Keys 30-60 are invisible; keys outside remain visible.
        expected = concat(range_inclusive(1, 29), range_inclusive(61, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_two_overlapping_truncated_ranges_scans_skip_union(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60 and 50-80.
        self.truncate(30, 60)
        self.truncate(50, 80)

        # Keys 30-80 are invisible; keys 1-29 and 81-100 remain visible.
        expected = concat(range_inclusive(1, 29), range_inclusive(81, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_two_disjoint_bounded_truncated_ranges(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 20-40 and 60-80.
        self.truncate(20, 40)
        self.truncate(60, 80)

        # Keys 20-40 and 60-80 are invisible; keys 1-19, 41-59, and 81-100
        # remain visible.
        expected = concat(
            range_inclusive(1, 19),
            range_inclusive(41, 59),
            range_inclusive(81, 100),
        )
        self.assertEqual(self.visible_keys(), expected)

    def test_bounded_combined_with_open_ended_truncate(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 20-40 and 80 onward.
        self.truncate(20, 40)
        self.truncate(80, None)

        # Keys 20-40 and 80 onward are invisible; keys 1-19 and 41-79 remain
        # visible.
        expected = concat(range_inclusive(1, 19), range_inclusive(41, 79))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_then_reinsert_within_same_transaction(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate keys 30-60 and reinsert key 45 within the same transaction.
        with self.transaction():
            with (
                self.cursor_for_key(30) as start,
                self.cursor_for_key(60) as stop,
                self.auto_closing_cursor() as cursor,
            ):
                self.session.truncate(None, start, stop, None)
                cursor[45] = "v"

        # Key 45 is visible; all other keys in 30-60 remain invisible.
        expected = concat(
            range_inclusive(1, 29), [45], range_inclusive(61, 100)
        )
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_then_reinsert_in_later_transaction(self):
        # Set up a follower with keys 1-100, then truncate keys 30-60.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))
        self.truncate(30, 60)

        # Reinsert key 45 in a later transaction.
        self.populate([45])

        # Key 45 is visible; all other keys in 30-60 remain invisible.
        expected = concat(
            range_inclusive(1, 29), [45], range_inclusive(61, 100)
        )
        self.assertEqual(self.visible_keys(), expected)


if __name__ == "__main__":
    wttest.run()
