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

# Truncate at read-committed or read-uncommitted isolation is rejected on a
# follower.

from contextlib import closing
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin, range_inclusive
from wiredtiger import WiredTigerError
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate23(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    """
    Truncate at read-committed or read-uncommitted isolation is rejected on a
    follower.
    """

    uris = [
        ("layered", {"uri": "layered:fast_truncate"}),
        ("table", {"uri": "table:fast_truncate"}),
    ]

    isolations = [
        ("read_committed", {"isolation": "read-committed"}),
        ("read_uncommitted", {"isolation": "read-uncommitted"}),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris, isolations)
    conn_config = 'disaggregated=(role="leader"),'

    ISOLATION_MSG = "/not supported in read-committed or read-uncommitted transactions/"

    def start_uncommitted_truncate(self, session):
        """Leave a truncate over 30-60 uncommitted on the given session."""
        with (
            closing(session.open_cursor(self.uri)) as start,
            closing(session.open_cursor(self.uri)) as stop,
        ):
            start.set_key(30)
            stop.set_key(60)
            session.begin_transaction()
            session.truncate(None, start, stop, None)

    def test_overlapping_truncate_with_ingest(self):
        # A follower with stable keys 1-100 and ingest key 45.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower(keys=[45])
        self.session.reconfigure('isolation=' + self.isolation)

        with closing(self.conn.open_session()) as session_a:
            self.start_uncommitted_truncate(session_a)
            self.assertRaisesWithMessage(
                WiredTigerError, lambda: self.truncate(40, 70), self.ISOLATION_MSG
            )
            self.session.rollback_transaction()
            session_a.rollback_transaction()

    def test_overlapping_truncate_no_ingest(self):
        # A follower with stable keys 1-100 and an empty ingest table.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()
        self.session.reconfigure('isolation=' + self.isolation)

        with closing(self.conn.open_session()) as session_a:
            self.start_uncommitted_truncate(session_a)
            self.assertRaisesWithMessage(
                WiredTigerError, lambda: self.truncate(40, 70), self.ISOLATION_MSG
            )
            self.session.rollback_transaction()
            session_a.rollback_transaction()

    def test_truncate_without_conflict(self):
        # A follower with stable keys 1-100 and no truncate in progress.
        self.setup_leader(keys=range_inclusive(1, 100))
        self.setup_follower()
        self.session.reconfigure('isolation=' + self.isolation)

        self.assertRaisesWithMessage(
            WiredTigerError, lambda: self.truncate(10, 20), self.ISOLATION_MSG
        )
        self.session.rollback_transaction()


if __name__ == "__main__":
    wttest.run()
