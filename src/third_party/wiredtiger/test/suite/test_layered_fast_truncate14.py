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

# test_layered_fast_truncate14.py
#   Ensure next() skips truncated stable keys after search_near lands on an ingest key.

from contextlib import closing
from typing import Iterable
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import disagg_fast_truncate_build
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate14(wttest.WiredTigerTestCase):
    """next() skips truncated stable keys after search_near lands on an ingest key."""

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
        return closing(self.session.open_cursor(self.uri))

    def populate(self, keys: Iterable[int]):
        with self.auto_closing_cursor() as cursor:
            with self.transaction():
                for key in keys:
                    cursor[key] = "v"

    def setup_leader(self, keys: Iterable[int] | None = None):
        self.session.create(self.uri, "key_format=i,value_format=S")
        if keys is not None:
            self.populate(keys)
        self.session.checkpoint()

    def setup_follower(self, keys: Iterable[int] | None = None):
        self.reopen_disagg_conn('disaggregated=(role="follower"),')
        if keys is not None:
            self.populate(keys)

    def truncate(self, start_key: int, stop_key: int):
        with (
            self.auto_closing_cursor() as start,
            self.auto_closing_cursor() as stop,
        ):
            start.set_key(start_key)
            stop.set_key(stop_key)
            with self.transaction():
                self.session.truncate(None, start, stop, None)

    def keys_after_search_near(self, search_key: int) -> list[int]:
        """
        Position on search_key via search_near (must be an exact match), then
        return all keys yielded by subsequent next() calls.
        """
        result = []
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                cursor.set_key(search_key)
                exact = cursor.search_near()
                self.assertEqual(exact, 0,
                    f"search_near({search_key}) must find an exact match")
                while cursor.next() == 0:
                    result.append(cursor.get_key())
        return result

    def test_next_after_search_near_skips_truncated_stable_key(self):
        # The first stable key after the ingest landing point is truncated.
        self.setup_leader(keys=[0, 10, 20, 30])
        self.setup_follower(keys=[5])
        self.truncate(10, 15)

        keys = self.keys_after_search_near(5)
        self.assertNotIn(10, keys,
            "truncated stable key 10 must not appear after search_near + next")
        self.assertEqual(keys, [20, 30])

    def test_next_skips_multiple_consecutive_truncated_stable_keys(self):
        # The truncate range spans two consecutive stable keys.
        self.setup_leader(keys=[0, 5, 10, 15, 20])
        self.setup_follower(keys=[3])
        self.truncate(5, 10)

        keys = self.keys_after_search_near(3)
        for k in [5, 10]:
            self.assertNotIn(k, keys,
                f"truncated stable key {k} must not appear after search_near + next")
        self.assertEqual(keys, [15, 20])

    def test_next_skips_truncated_stable_key_when_ingest_key_is_adjacent(self):
        # The ingest key sits just below the truncate range start.
        self.setup_leader(keys=[0, 10, 20, 30])
        self.setup_follower(keys=[7])
        self.truncate(10, 15)

        keys = self.keys_after_search_near(7)
        self.assertNotIn(10, keys,
            "truncated stable key 10 must not appear after search_near + next")
        self.assertEqual(keys, [20, 30])

    def test_next_skips_truncated_gap_past_multiple_ingest_keys(self):
        # Multiple ingest keys are visited before reaching the truncated gap.
        self.setup_leader(keys=[0, 10, 20, 30, 40])
        self.setup_follower(keys=[5, 15])
        self.truncate(20, 25)

        keys = self.keys_after_search_near(5)
        self.assertNotIn(20, keys,
            "truncated stable key 20 must not appear after search_near + next")
        self.assertEqual(keys, [10, 15, 30, 40])


if __name__ == "__main__":
    wttest.run()
