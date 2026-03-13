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
# test_cursor24.py
#   Test version cursor prepare metadata fields and rollback visibility.
#
import wttest
import wiredtiger
from wtscenario import make_scenarios

WT_TS_MAX = 2**64 - 1
WT_UPDATE_PREPARE_ROLLBACK = 0x080

class test_cursor24(wttest.WiredTigerTestCase):
    uri = 'file:test_cursor24.wt'

    types = [
        ('row', dict(keyformat='i', valueformat='i')),
        ('var', dict(keyformat='r', valueformat='i')),
    ]

    scenarios = make_scenarios(types)

    def create(self):
        self.session.create(self.uri,
            'key_format={},value_format={}'.format(self.keyformat, self.valueformat))

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

    def verify_prepare_fields(self, version_cursor, expected_start_prepare_ts,
                              expected_stop_prepare_ts):
        values = self.get_values(version_cursor)
        self.assertEqual(values[3], expected_start_prepare_ts)
        self.assertEqual(values[8], expected_stop_prepare_ts)

    def open_version_cursor(self, visible_only=False):
        internal_config = ["enabled=true"]
        if visible_only:
            internal_config.append("visible_only=true")
        config = "debug=(dump_version=(" + ",".join(internal_config) + "))"
        return self.session.open_cursor(self.uri, None, config)

    def test_prepare_commit_metadata(self):
        """Verify start_prepare_ts and start_ts/start_durable_ts after prepare + commit."""
        self.create()

        session2 = self.conn.open_session()
        cursor = session2.open_cursor(self.uri, None)

        session2.begin_transaction()
        cursor[1] = 0
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(5))
        session2.commit_transaction(
            "commit_timestamp=" + self.timestamp_str(10) +
            ",durable_timestamp=" + self.timestamp_str(15))

        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)

        # start_ts=10 (commit), start_durable_ts=15 (durable), start_prepare_ts=5
        self.verify_value(version_cursor, 10, 15, WT_TS_MAX, 0, 3, 0, 0, 0, 0)
        self.verify_prepare_fields(version_cursor, 5, WT_TS_MAX)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_prepare_commit_tombstone_metadata(self):
        """Verify stop_prepare_ts is set when a prepared tombstone stops a value."""
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        # Evict to disk so the value is on-disk.
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        self.assertEqual(evict_cursor[1], 0)
        evict_cursor.reset()
        self.session.rollback_transaction()

        # Delete the value via a prepared transaction, then commit it.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2.set_key(1)
        self.assertEqual(cursor2.remove(), 0)
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(5))
        session2.commit_transaction(
            "commit_timestamp=" + self.timestamp_str(10) +
            ",durable_timestamp=" + self.timestamp_str(15))

        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)
        self.assertEqual(version_cursor.get_key(), 1)

        # The ondisk value at ts=1 is stopped by the committed prepared tombstone.
        # stop_ts=10 (commit), stop_durable_ts=15 (durable), stop_prepare_ts=5
        self.verify_value(version_cursor, 1, 1, 10, 15, 3, 0, 0, 1, 0)
        self.verify_prepare_fields(version_cursor, 0, 5)
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_prepare_rollback_key_not_found(self):
        """
        When the only update on a key was via a prepare that got rolled back, the version
        cursor should return NOTFOUND since the rollback tombstone is globally visible and
        there is no committed value underneath.
        """
        self.create()

        # Insert via prepare only (no prior committed value), then roll back.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 99
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(5))
        session2.rollback_transaction()

        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), wiredtiger.WT_NOTFOUND)

    def test_prepare_rollback_then_update(self):
        """
        After prepare rollback on a key with no prior committed value, a subsequent committed
        insert should be the only version visible via the version cursor since the rollback
        tombstone (globally visible) acts as a terminal stop.
        """
        self.create()

        # Insert via prepare only (no prior committed value), then roll back.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri, None)
        session2.begin_transaction()
        cursor2[1] = 99
        session2.prepare_transaction("prepare_timestamp=" + self.timestamp_str(5))
        session2.rollback_transaction()

        # Write a new committed value on the same key.
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 2
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))

        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)

        # Only the committed value at ts=10 should be visible.
        self.assertEqual(version_cursor.get_key(), 1)
        self.verify_value(version_cursor, 10, 10, WT_TS_MAX, 0, 3, 0, 0, 0, 2)
        self.verify_prepare_fields(version_cursor, 0, WT_TS_MAX)

        # The rollback tombstone terminates the chain; no more versions.
        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)

    def test_non_prepared_zero_fields(self):
        """Non-prepared updates should have start_prepare_ts=0 and appropriate stop_prepare_ts."""
        self.create()

        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor[1] = 0
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(1))

        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))

        self.session.begin_transaction()
        version_cursor = self.open_version_cursor()
        version_cursor.set_key(1)
        self.assertEqual(version_cursor.search(), 0)

        # Newest value: start_prepare_ts=0, stop_prepare_ts=WT_TS_MAX (no stop)
        self.verify_value(version_cursor, 5, 5, WT_TS_MAX, 0, 3, 0, 0, 0, 1)
        self.verify_prepare_fields(version_cursor, 0, WT_TS_MAX)

        # Older value: start_prepare_ts=0, stop_prepare_ts=0 (stopped by non-prepared update)
        self.assertEqual(version_cursor.next(), 0)
        self.verify_value(version_cursor, 1, 1, 5, 5, 3, 0, 0, 0, 0)
        self.verify_prepare_fields(version_cursor, 0, 0)

        self.assertEqual(version_cursor.next(), wiredtiger.WT_NOTFOUND)
