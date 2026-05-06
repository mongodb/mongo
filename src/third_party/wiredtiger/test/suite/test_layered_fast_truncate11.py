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

# test_layered_fast_truncate11.py
#   Range specification (start / end / open-ended).
#
#   Verify that follower fast-truncate handles all range-bound variations
#   correctly: open ends, single keys, empty gaps, and invalid ordering.
#   Open-ended truncates should not apply to keys written after the truncate
#   commits.

from contextlib import closing, nullcontext
from itertools import chain
from typing import Iterable
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import WiredTigerError, disagg_fast_truncate_build
from wtscenario import make_scenarios
import wttest


def concat(*iterables: Iterable[int]) -> list[int]:
    """Concatenate any number of iterables into a single list."""
    return list(chain.from_iterable(iterables))


def range_inclusive(start: int, stop: int) -> range:
    """Return a range covering [start, stop] inclusive."""
    return range(start, stop + 1)


@disagg_test_class
class test_layered_fast_truncate11(wttest.WiredTigerTestCase):
    """
    Range specification (start / end / open-ended).

    Verify that follower fast-truncate handles all range-bound variations
    correctly: open ends, single keys, empty gaps, and invalid ordering.
    Open-ended truncates should not apply to keys written after the truncate
    commits.
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

    def auto_closing_cursor(self) -> closing:
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

    def cursor_for_key(self, key: int | None):
        """Return a cursor with its key set, or None if key is None."""
        if key is None:
            return nullcontext(None)  # Open-ended truncate.
        cursor = self.auto_closing_cursor()
        cursor.thing.set_key(key)
        return cursor

    def truncate(self, start_key: int | None, stop_key: int | None):
        """Truncate [start_key, stop_key] inclusive; None means open end."""
        with (
            self.cursor_for_key(start_key) as start,
            self.cursor_for_key(stop_key) as stop,
        ):
            # WT requires a URI when both cursors are absent.
            uri = self.uri if (start is None and stop is None) else None
            with self.transaction():
                self.session.truncate(uri, start, stop, None)

    def visible_keys(self) -> list[int]:
        """Return all keys visible via a forward scan, in key order."""
        result = []
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                while cursor.next() == 0:
                    result.append(cursor.get_key())
        return result

    def test_truncate_with_null_start_key(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate from the beginning of the table up to key 60.
        self.truncate(None, 60)

        # Only keys beyond the truncated range should remain.
        self.assertEqual(self.visible_keys(), list(range_inclusive(61, 100)))

    def test_truncate_with_null_end_key(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate from key 30 to the end of the table.
        self.truncate(30, None)

        # Only keys before the truncated range should remain.
        self.assertEqual(self.visible_keys(), list(range_inclusive(1, 29)))

    def test_truncate_with_both_null(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate the entire table with NULL start and NULL end.
        self.truncate(None, None)

        # No keys should remain visible.
        self.assertEqual(self.visible_keys(), [])

    def test_truncate_range_with_no_matching_keys(self):
        # Set up a follower with a gap in the key space (21-79 missing).
        self.setup_leader()
        self.setup_follower(
            keys=concat(range_inclusive(1, 20), range_inclusive(80, 100))
        )

        # Truncate the gap where no keys exist.
        self.truncate(30, 60)

        # All original keys should remain visible since none were in range.
        expected = concat(range_inclusive(1, 20), range_inclusive(80, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_single_key(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncate a single key by setting start == end.
        self.truncate(45, 45)

        # Only key 45 should be invisible; all others remain.
        expected = concat(range_inclusive(1, 44), range_inclusive(46, 100))
        self.assertEqual(self.visible_keys(), expected)

    def test_truncate_with_start_greater_than_end(self):
        # Set up a follower with keys 1-100.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))

        # Truncating with start > end should be rejected with an error.
        self.assertRaisesWithMessage(
            WiredTigerError,
            lambda: self.truncate(60, 30),
            "/Invalid argument/",
        )
        self.session.rollback_transaction()

        # All original keys should still be visible.
        self.assertEqual(self.visible_keys(), list(range_inclusive(1, 100)))

    def test_open_ended_truncate_does_not_cover_later_appends(self):
        # Set up a follower with keys 1-100, then truncate from key 80 onward.
        self.setup_leader()
        self.setup_follower(keys=range_inclusive(1, 100))
        self.truncate(80, None)

        # Insert keys well beyond the original range after the truncate commits.
        self.populate(range_inclusive(200, 210))

        # Keys 1-79 survive; 200-210 are visible as new data; 80-100 remain
        # invisible. The open-ended truncate does not cover keys that did not
        # exist at commit time.
        expected = concat(range_inclusive(1, 79), range_inclusive(200, 210))
        self.assertEqual(self.visible_keys(), expected)


if __name__ == "__main__":
    wttest.run()
