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

from contextlib import closing, contextmanager
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
import wttest
from wiredtiger import WiredTigerError


@disagg_test_class
class test_layered_prepare10(wttest.WiredTigerTestCase):
    """
    A reader stalls mid-walk on a prepare transaction, leaving the ingest cursor with no key but
    still positioned. While it is stalled, the key that the stable cursor is on is deleted by a
    newer checkpoint that the follower picks up. Once the conflict resolves, the walk must still
    return every other visible key.
    """

    uri = f"layered:{__qualname__}"
    BASE_CONFIG = "statistics=(all),precise_checkpoint=true,preserve_prepared=true"
    conn_config = BASE_CONFIG + ',disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)

    STABLE_KEYS = (4, 6, 8)
    INGEST_KEYS = (1, 5, 7, 11)

    # The prepared key sits just before the stable range (either below it for forward or above it
    # for backward). This is to demonstrate the worst case where no stable keys have been returned
    # to the user yet.
    scenarios = make_scenarios(
        disagg_storages,
        [
            ("forward", dict(forward=True, prepared_key=3)),
            ("backward", dict(forward=False, prepared_key=9)),
        ],
    )

    @property
    def walk_order(self):
        return sorted((*self.INGEST_KEYS, *self.STABLE_KEYS), reverse=not self.forward)

    def setUp(self):
        super().setUp()
        self._setup_leader()
        self._setup_follower()

    def _setup_leader(self):
        oldest = self.timestamp_str(10)
        self.conn.set_timestamp(f"oldest_timestamp={oldest},stable_timestamp={oldest}")
        self.session.create(self.uri, "key_format=i,value_format=S")
        stable_ts = 20
        for key in self.STABLE_KEYS:
            self._commit(self.session, key, "stable", stable_ts)
        self._leader_checkpoint(stable_ts)

    def _follower_session(self):
        return closing(self.follower.open_session())

    def _setup_follower(self):
        self.follower = self.wiredtiger_open(
            "follower",
            f"{self.extensionsConfig()},create,{self.BASE_CONFIG},"
            f'disaggregated=(role="follower")',
        )
        self.disagg_advance_checkpoint(self.follower)
        with self._follower_session() as session:
            for key in self.INGEST_KEYS:
                self._commit(session, key, "ingest", 22)

    def _commit(self, session, key, value, ts):
        with (
            wttest.open_cursor(session, self.uri) as cursor,
            self.transaction(session=session, commit_timestamp=ts),
        ):
            cursor[key] = value

    def _remove(self, session, key, ts):
        with (
            wttest.open_cursor(session, self.uri, config='overwrite=false') as cursor,
            self.transaction(session=session, commit_timestamp=ts),
        ):
            cursor.set_key(key)
            self.assertEqual(cursor.remove(), 0)

    def _leader_checkpoint(self, ts):
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(ts)}")
        self.session.checkpoint()

    def _start_prepare_txn(self, session):
        session.begin_transaction()
        with wttest.open_cursor(session, self.uri) as cursor:
            cursor[self.prepared_key] = "prepared"
        session.prepare_transaction(
            f"prepare_timestamp={self.timestamp_str(25)},"
            f"prepared_id={self.prepared_id_str(1)}"
        )

    def _rollback_prepare_txn(self, session):
        session.rollback_transaction(f"rollback_timestamp={self.timestamp_str(35)}")

    @contextmanager
    def _during_prepare_txn_stall(self, read_ts, seen_keys):
        """
        Stall a reader on the prepared key for the relevant test. On exit, rollback the prepare txn
        and record the rest of the cursor walk into seen_keys.
        """

        with (
            self._follower_session() as prepared_session,
            self._follower_session() as read_session,
            wttest.open_cursor(read_session, self.uri) as read_cursor,
        ):
            self._start_prepare_txn(prepared_session)
            read_session.begin_transaction(
                f"read_timestamp={self.timestamp_str(read_ts)}"
            )

            # Stall the reader on the prepare conflict.
            step = read_cursor.next if self.forward else read_cursor.prev
            self.assertEqual(step(), 0)
            seen_keys.append(read_cursor.get_key())
            self.assertRaisesException(
                WiredTigerError, step, "/conflict with a prepared update/"
            )

            yield

            self._rollback_prepare_txn(prepared_session)
            while step() == 0:
                seen_keys.append(read_cursor.get_key())
            read_session.rollback_transaction()

    def test_reopen_notfound_while_ingest_stalled(self):
        """The key that the stable cursor is on is deleted in a newer checkpoint."""

        # The first key (depending on direction) is what the stable cursor will be positioned on.
        parked_stable_key = self.STABLE_KEYS[0] if self.forward else self.STABLE_KEYS[-1]
        delete_ts = 28

        # Simulate the oplog delete landing in the follower's ingest before the read (a tombstone),
        # so the key stays masked regardless of the stable checkpoint.
        with self._follower_session() as session:
            self._remove(session, parked_stable_key, delete_ts)

        read_ts = 30
        seen_keys = []
        with self._during_prepare_txn_stall(read_ts, seen_keys):
            self._remove(self.session, parked_stable_key, delete_ts)
            self._leader_checkpoint(read_ts)
            self.disagg_advance_checkpoint(self.follower)

        # The deleted key should not be visible.
        expected = [k for k in self.walk_order if k != parked_stable_key]
        self.assertEqual(seen_keys, expected, "the walk lost a visible key")


if __name__ == "__main__":
    wttest.run()
