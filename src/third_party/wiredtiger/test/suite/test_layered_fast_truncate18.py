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

# test_layered_fast_truncate18.py
#   Write conflict detection for follower fast truncate (truncate-truncate
#   conflicts only).

import unittest
from contextlib import closing, nullcontext
from typing import Iterable
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import WiredTigerError
from wtscenario import make_scenarios
import wttest


def range_inclusive(start: int, stop: int) -> range:
    """Return a range covering [start, stop] inclusive."""
    return range(start, stop + 1)


@disagg_test_class
class test_layered_fast_truncate18(wttest.WiredTigerTestCase):
    """
    Write conflict detection for follower fast truncate (truncate-truncate
    conflicts only).
    """

    uris = [
        ("layered", {"uri": "layered:fast_truncate"}),
        ("table", {"uri": "table:fast_truncate"}),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)
    conn_config = 'disaggregated=(role="leader"),'

    CONFLICT_MSG = "/conflict between concurrent operations/"

    def session_create_config(self) -> str:
        """Return a config string for session.create() based on table URI."""
        cfg = "key_format=i,value_format=S"
        if self.uri.startswith("table"):
            cfg += ",block_manager=disagg,type=layered"
        return cfg

    def auto_closing_cursor(self, session) -> closing:
        """Return a cursor that auto-closes as it goes out of scope."""
        return closing(session.open_cursor(self.uri))

    def auto_closing_session(self) -> closing:
        """Return a session that auto-closes as it goes out of scope."""
        return closing(self.conn.open_session())

    def populate(self, keys: Iterable[int]):
        """Insert each key with a placeholder value in a single transaction."""
        with self.auto_closing_cursor(self.session) as cursor:
            with self.transaction():
                for key in keys:
                    cursor[key] = "v"

    def setup_leader(self, keys: Iterable[int] | None = None):
        """Create the table on the leader and optionally populate stable."""
        self.session.create(self.uri, self.session_create_config())
        if keys is not None:
            self.populate(keys)
        self.session.checkpoint()

    def setup_follower(self, keys: Iterable[int] | None = None):
        """Switch to follower role and optionally write keys to ingest."""
        self.reopen_disagg_conn('disaggregated=(role="follower"),')
        if keys is not None:
            self.populate(keys)

    def cursor_for_key(self, key: int | None, session):
        """Return a cursor with its key set, or None if key is None."""
        if key is None:
            return nullcontext(None)
        cursor = self.auto_closing_cursor(session)
        cursor.thing.set_key(key)
        return cursor

    def truncate(self, session, start_key: int | None, stop_key: int | None):
        """Execute a truncate from start to stop key inclusive."""
        with (
            self.cursor_for_key(start_key, session) as start,
            self.cursor_for_key(stop_key, session) as stop,
        ):
            uri = self.uri if (start is None and stop is None) else None
            session.truncate(uri, start, stop, None)

    def test_same_txn_truncates_no_self_conflict(self):
        # A follower with stable keys 1-100.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # Within a single transaction: truncate 30-60, then truncate 40-80.
        with self.transaction():
            self.truncate(self.session, 30, 60)
            self.truncate(self.session, 40, 80)

        # The transaction committed; no WT_ROLLBACK raised.

    def test_overlapping_truncates_conflict_with_ingest(self):
        # A follower with stable keys 1-100 and ingest key 45.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower(keys=[45])

        # txn A begins a truncate over 30-60 and leaves it uncommitted.
        session_a = self.session
        session_a.begin_transaction()
        self.truncate(session_a, 30, 60)

        # txn B truncates overlapping range 40-70 and gets WT_ROLLBACK.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(session=session_b, rollback=True),
        ):
            self.assertRaisesException(
                WiredTigerError,
                lambda: self.truncate(session_b, 40, 70),
                self.CONFLICT_MSG,
            )

    def test_overlapping_truncates_conflict_no_ingest(self):
        # A follower with stable keys 1-100 and an empty ingest table.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # txn A begins a truncate over 30-60 and leaves it uncommitted.
        session_a = self.session
        session_a.begin_transaction()
        self.truncate(session_a, 30, 60)

        # txn B truncates overlapping range 40-70 and gets WT_ROLLBACK.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(session=session_b, rollback=True),
        ):
            self.assertRaisesException(
                WiredTigerError,
                lambda: self.truncate(session_b, 40, 70),
                self.CONFLICT_MSG,
            )

    def test_non_overlapping_truncates_no_conflict(self):
        # A follower with stable keys 1-100.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # txn A truncates 10-30 and leaves it uncommitted.
        session_a = self.session
        session_a.begin_transaction()
        self.truncate(session_a, 10, 30)

        # txn B truncates 50-70 (no overlap) and commits successfully.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(session=session_b),
        ):
            self.truncate(session_b, 50, 70)

    def test_rolled_back_truncate_no_residual(self):
        # A follower with stable keys 1-100.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # txn A truncates 30-60 then explicitly rolls back.
        session_a = self.session
        with self.transaction(session=session_a, rollback=True):
            self.truncate(session_a, 30, 60)

        # txn B truncates the same range 30-60 and commits without WT_ROLLBACK.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(session=session_b),
        ):
            self.truncate(session_b, 30, 60)

    def test_invisible_committed_truncate_conflicts(self):
        # A follower with stable keys 1-100.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # txn A commits a truncate over 30-60 at ts=10 (invisible to txn B).
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))
        with self.transaction(commit_timestamp=10):
            self.truncate(self.session, 30, 60)

        # txn B (read_ts=5) truncates overlapping range 40-70 and gets
        # WT_ROLLBACK.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(
                session=session_b, read_timestamp=5, rollback=True
            ),
        ):
            self.assertRaisesException(
                WiredTigerError,
                lambda: self.truncate(session_b, 40, 70),
                self.CONFLICT_MSG,
            )

    def test_visible_committed_truncate_no_conflict(self):
        # A follower with stable keys 1-100.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()

        # txn A commits a truncate over 30-60 at ts=5 (visible to txn B).
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))
        with self.transaction(commit_timestamp=5):
            self.truncate(self.session, 30, 60)

        # txn B (read_ts=10) truncates overlapping range 40-70 without
        # WT_ROLLBACK.
        with (
            self.auto_closing_session() as session_b,
            self.transaction(session=session_b, read_timestamp=10),
        ):
            self.truncate(session_b, 40, 70)


if __name__ == "__main__":
    wttest.run()
