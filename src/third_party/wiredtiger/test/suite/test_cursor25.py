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
#
# test_cursor25.py
#   Test version cursor show_prepared_rollback config across prepared insert,
#   update, and delete rollback scenarios.
#
import wttest
import wiredtiger
from wtscenario import make_scenarios

WT_TS_MAX = 2**64 - 1
WT_TXN_MAX = 2**64 - 11
WT_TXN_ABORTED = 2**64 - 1
PREPARE_TS = 5
ROLLBACK_TS = 15

class test_cursor25(wttest.WiredTigerTestCase):
    uri = 'file:test_cursor25.wt'

    types = [
        ('row', dict(keyformat='i', valueformat='i')),
        ('var', dict(keyformat='r', valueformat='i')),
    ]

    scenarios = make_scenarios(types)

    def create(self):
        self.session.create(self.uri,
            'key_format={},value_format={},in_memory=true,log=(enabled=false)'.format(
                self.keyformat, self.valueformat))

    # Format: start_txn(0), start_ts(1), start_durable_ts(2), start_prepare_ts(3),
    #   start_prepared_id(4), stop_txn(5), stop_ts(6), stop_durable_ts(7),
    #   stop_prepare_ts(8), stop_prepared_id(9), type(10), prepare(11),
    #   flags(12), location(13), value(14)
    def get_values(self, version_cursor):
        return version_cursor.get_values()

    def verify_value(self, version_cursor, expected_start_ts, expected_start_durable_ts,
                     expected_stop_ts, expected_stop_durable_ts, expected_type,
                     expected_prepare_state, expected_flags, expected_location, expected_value):
        values = self.get_values(version_cursor)
        self.assertEqual(values[1], expected_start_ts)
        self.assertEqual(values[2], expected_start_durable_ts)
        self.assertEqual(values[6], expected_stop_ts)
        self.assertEqual(values[7], expected_stop_durable_ts)
        self.assertEqual(values[10], expected_type)
        self.assertEqual(values[11], expected_prepare_state)
        self.assertEqual(values[12], expected_flags)
        self.assertEqual(values[13], expected_location)
        self.assertEqual(values[14], expected_value)

    def verify_prepare_rollback_value(self, version_cursor, expected_value, expected_rollback_ts):
        values = self.get_values(version_cursor)
        self.assertEqual(values[0], WT_TXN_ABORTED)
        self.assertEqual(values[2], expected_rollback_ts)
        self.assertEqual(values[10], 3)  # WT_UPDATE_STANDARD
        self.assertEqual(values[11], 1)  # prepared
        self.assertEqual(values[13], 0)  # WT_CURVERSION_UPDATE_CHAIN
        self.assertEqual(values[14], expected_value)

    def open_version_cursor(self, visible_only=False, show_prepared_rollback=False):
        internal_config = ["enabled=true"]
        if visible_only:
            internal_config.append("visible_only=true")
        if show_prepared_rollback:
            internal_config.append("show_prepared_rollback=true")
        config = "debug=(dump_version=(" + ",".join(internal_config) + "))"
        return self.session.open_cursor(self.uri, None, config)

    def prepared_insert_rollback(self, key, value, prepare_ts, rollback_ts):
        """Insert via a prepared transaction and roll back."""
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[key] = value
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(prepare_ts))
        session2.rollback_transaction("rollback_timestamp=" + self.timestamp_str(rollback_ts))
        cursor2.close()
        session2.close()

    def test_rollback_insert_only(self):
        """
        Prepared insert (no prior value) + rollback with no subsequent write.
        For in-memory trees, show_prepared_rollback emits the rolled-back prepared value.
        """
        self.create()
        self.prepared_insert_rollback(1, 99, PREPARE_TS, ROLLBACK_TS)

        # Without flag: NOTFOUND.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: rolled-back prepared value is emitted.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_insert_then_committed_insert(self):
        """
        Prepared insert (no prior value) + rollback, then a new committed insert.
        With show_prepared_rollback=true, emit committed insert first, then rolled-back
        prepared insert.
        """
        self.create()
        self.prepared_insert_rollback(1, 99, PREPARE_TS, ROLLBACK_TS)

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 42
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # Without flag: committed value emitted, rollback tombstone skipped.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: committed insert first, then rolled-back value.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_update_over_committed(self):
        """
        Committed insert@ts=1, then prepared insert (overwrite)@ts=5 + rollback.
        Chain: [aborted prepared insert] -> [committed insert@ts=1].
        With show_prepared_rollback=true, both the rolled-back prepared insert and the
        committed value are emitted.
        """
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 10
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Prepared overwrite + rollback. No PREPARE_ROLLBACK tombstone because
        # first_committed_upd != NULL.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 20
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(PREPARE_TS))
        session2.rollback_transaction("rollback_timestamp=" + self.timestamp_str(ROLLBACK_TS))
        cursor2.close()
        session2.close()

        # Without flag: aborted insert skipped, committed value emitted.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 0, 10)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: emit rolled-back prepared insert, then committed value.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_prepare_rollback_value(vc, 20, ROLLBACK_TS)
        self.assertEqual(vc.next(), 0)
        values = self.get_values(vc)
        self.assertEqual(values[10], 3)  # WT_UPDATE_STANDARD
        self.assertEqual(values[14], 10)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_delete_over_committed(self):
        """
        Committed insert@ts=1, then prepared delete@ts=5 + rollback.
        Chain: [aborted prepared tombstone] -> [committed insert@ts=1].
        show_prepared_rollback should not emit a tombstone-only rollback row.
        """
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 10
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Prepared delete + rollback. No PREPARE_ROLLBACK tombstone because
        # first_committed_upd != NULL.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2.set_key(1)
        self.assertEqual(cursor2.remove(), 0)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(PREPARE_TS))
        session2.rollback_transaction("rollback_timestamp=" + self.timestamp_str(ROLLBACK_TS))
        cursor2.close()
        session2.close()

        # Without flag: aborted tombstone skipped, committed value emitted.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 0, 10)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: same result. The head rollback is a tombstone, and this
        # test only expects value rows to be emitted.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 1, 1, WT_TS_MAX, 0, 3, 0, 0, 0, 10)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_insert_then_committed_update(self):
        """
        Prepared insert (no prior value) + rollback, then committed insert@ts=10,
        then committed update@ts=20.
        With show_prepared_rollback=true, emit two committed versions, then the
        rolled-back prepared insert.
        """
        self.create()
        self.prepared_insert_rollback(1, 99, PREPARE_TS, ROLLBACK_TS)

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 42
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        self.session.begin_transaction()
        cursor[1] = 84
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(20))

        # Without flag: both committed versions emitted, rollback tombstone skipped.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 20, 20, WT_TS_MAX, 0, 3, 0, 0, 0, 84)
        self.assertEqual(vc.next(), 0)
        self.verify_value(vc, 10, 10, 20, 20, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: two committed versions, then rolled-back value.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 20, 20, WT_TS_MAX, 0, 3, 0, 0, 0, 84)
        self.assertEqual(vc.next(), 0)
        self.verify_value(vc, 10, 10, 20, 20, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_insert_delete_same_txn(self):
        """
        A prepared transaction inserts then deletes the same key (no prior value), then rolls back.
        show_prepared_rollback emits the rolled-back prepared value update, not
        tombstone rollback markers.
        Then add a committed insert and verify it is emitted.
        """
        self.create()

        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 99
        cursor2.set_key(1)
        self.assertEqual(cursor2.remove(), 0)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(PREPARE_TS))
        session2.rollback_transaction("rollback_timestamp=" + self.timestamp_str(ROLLBACK_TS))
        cursor2.close()
        session2.close()

        # Without flag: NOTFOUND.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: emit the rolled-back prepared insert.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # Now add a committed insert on the same key.
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 42
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # The committed value is visible without show_prepared_rollback.
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

        # With show_prepared_rollback: committed value first, then rolled-back value.
        self.session.begin_transaction()
        vc = self.open_version_cursor(show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        self.verify_value(vc, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 42)
        self.assertEqual(vc.next(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_rollback_insert_flag_without_visible_only(self):
        """
        Same as test_rollback_insert_then_committed_insert but opens the version cursor
        with show_prepared_rollback=true and visible_only=false (the default).
        Committed row is emitted first, then the rolled-back prepared row.
        """
        self.create()
        self.prepared_insert_rollback(1, 99, PREPARE_TS, ROLLBACK_TS)

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 42
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        # show_prepared_rollback=true with visible_only=false (default).
        self.session.begin_transaction()
        vc = self.open_version_cursor(visible_only=False, show_prepared_rollback=True)
        vc.set_key(1)
        self.assertEqual(vc.search(), 0)
        values = self.get_values(vc)
        self.assertEqual(values[14], 42)
        self.assertEqual(values[1], 10)
        self.assertEqual(values[2], 10)
        self.assertEqual(vc.next(), 0)
        self.verify_prepare_rollback_value(vc, 99, ROLLBACK_TS)
        self.assertEqual(vc.next(), wiredtiger.WT_NOTFOUND)
        vc.close()
        self.session.rollback_transaction()

    def test_show_prepared_rollback_requires_in_memory(self):
        uri = 'file:test_cursor25_non_in_memory.wt'
        self.session.create(uri,
            'key_format={},value_format={}'.format(self.keyformat, self.valueformat))
        msg = '/show_prepared_rollback is only supported for in-memory b-trees/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
                uri, None, 'debug=(dump_version=(enabled=true,show_prepared_rollback=true))'),
            msg)
