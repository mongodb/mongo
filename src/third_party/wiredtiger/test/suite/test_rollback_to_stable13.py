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
from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable13.py
# Test the rollback to stable should retain/restore the tombstone from
# the update list or from the history store for on-disk database.
class test_rollback_to_stable13(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    scenarios = make_scenarios(format_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),log=(enabled=true)'
        return config

    def test_rollback_to_stable(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable13"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='split_pct=50,log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)

        # Perform several removes.
        self.large_removes(uri, ds, nrows, self.prepare, 30)

        # Perform several updates.
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        # (In FLCS, the removed rows should read back as zero.)
        self.check(value_a, uri, nrows, None, 20)
        self.check(None, uri, 0, nrows, 30)
        self.check(value_b, uri, nrows, None, 60)

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        self.session.checkpoint()
        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(None, uri, 0, nrows, 50)

        # Check that we restore the correct value from the history store.
        self.check(value_a, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        restored_tombstones = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        self.assertEqual(restored_tombstones, nrows)

    def test_rollback_to_stable_with_aborted_updates(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable13"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='split_pct=50,log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100
            value_d = "ddddd" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)

        # Update a large number of records and rollback.
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_b
            self.session.rollback_transaction()
        cursor.close()

        # Update a large number of records and rollback.
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_c
            self.session.rollback_transaction()
        cursor.close()

        # Perform several removes.
        self.large_removes(uri, ds, nrows, self.prepare, 30)

        # Perform several updates.
        self.large_updates(uri, value_d, ds, nrows, self.prepare, 60)

        # Verify data is visible and correct.
        # (In FLCS, the removed rows should read back as zero.)
        self.check(value_a, uri, nrows, None, 20)
        self.check(None, uri, 0, nrows, 30)
        self.check(value_d, uri, nrows, None, 60)

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        self.session.checkpoint()
        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(None, uri, 0, nrows, 50)

        # Check that we restore the correct value from the history store.
        self.check(value_a, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        restored_tombstones = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        self.assertEqual(restored_tombstones, nrows)

    def test_rollback_to_stable_with_history_tombstone(self):
        nrows = 1000

        # Create a table without logging.
        uri = "table:rollback_to_stable13"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='split_pct=50,log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)

        # Perform several removes.
        self.large_removes(uri, ds, nrows, self.prepare, 30)

        # Perform several updates and removes in a single transaction.
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_b
            cursor.set_key(ds.key(i))
            cursor.remove()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))
        cursor.close()

        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        self.session.checkpoint()

        # Perform several updates and checkpoint.
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 60)
        self.session.checkpoint()

        # Verify data is visible and correct.
        # (In FLCS, the removed rows should read back as zero.)
        self.check(value_a, uri, nrows, None, 20)
        self.check(None, uri, 0, nrows, 40)
        self.check(value_c, uri, nrows, None, 60)

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        # Check that the correct data is seen at and after the stable timestamp.
        self.check(None, uri, 0, nrows, 50)

        # Check that we restore the correct value from the history store.
        self.check(value_a, uri, nrows, None, 20)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        restored_tombstones = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        self.assertEqual(restored_tombstones, nrows)

    def test_rollback_to_stable_with_stable_remove(self):
        nrows = 1000
        # Create a table without logging.
        uri = "table:rollback_to_stable13"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='split_pct=50,log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.large_updates(uri, value_a, ds, nrows, self.prepare, 20)
        # Perform several updates.
        self.large_updates(uri, value_b, ds, nrows, self.prepare, 30)
        # Perform several removes.
        self.large_removes(uri, ds, nrows, self.prepare, 40)
        # Pin stable to timestamp 50 if prepare otherwise 40.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        # Perform several updates and checkpoint.
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 60)
        self.session.checkpoint()

        # Verify data is visible and correct.
        # (In FLCS, the removed rows should read back as zero.)
        self.check(value_a, uri, nrows, None, 20)
        self.check(None, uri, 0, nrows, 40)
        self.check(value_c, uri, nrows, None, 60)

        self.conn.rollback_to_stable()
        # Perform several updates and checkpoint.
        self.large_updates(uri, value_c, ds, nrows, self.prepare, 60)
        self.session.checkpoint()
        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")
        # Check that the correct data is seen at and after the stable timestamp.
        self.check(None, uri, 0, nrows, 50)
        # Check that we restore the correct value from the history store.
        self.check(value_a, uri, nrows, None, 20)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        restored_tombstones = stat_cursor[stat.conn.txn_rts_hs_restore_tombstones][2]
        self.assertEqual(restored_tombstones, nrows)
