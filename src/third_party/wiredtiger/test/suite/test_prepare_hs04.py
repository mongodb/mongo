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

from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_prepare_hs04.py
# Read prepared updates from on-disk with ignore_prepare.
# Committing or aborting a prepared update when there exists a tombstone for that key already.
#
class test_prepare_hs04(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=5MB,statistics=(fast)'

    # Create a small table.
    uri = "table:test_prepare_hs04"

    nsessions = 3
    nkeys = 40
    nrows = 100

    commit_values = [
        ('commit_transaction', dict(commit=True)),
        ('rollback_transaction', dict(commit=False))
    ]

    format_values = [
        # Note: commit_key must exceed nrows to give behavior comparable to the row case.
        ('column', dict(key_format='r', commit_key=1000, value_format='u')),
        ('column-fix', dict(key_format='r', commit_key=1000, value_format='8t')),
        ('string-row', dict(key_format='S', commit_key='C', value_format='u')),
    ]

    scenarios = make_scenarios(commit_values, format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def search_keys_timestamp_and_ignore(self, ds, txn_config, expected_value, conflict=False):
        cursor = self.session.open_cursor(self.uri)

        commit_key = self.commit_key
        self.session.begin_transaction(txn_config)
        for i in range(1, self.nsessions * self.nkeys):
            key = commit_key + ds.key(self.nrows + i)
            cursor.set_key(key)
            if conflict == True:
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:cursor.search(), expected_value)
            elif expected_value == None:
                if self.value_format == '8t':
                    # In FLCS, deleted values read back as 0.
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), 0)
                else:
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(cursor.search(), 0)
                self.assertEqual(cursor.get_value(), expected_value)
        cursor.close()
        self.session.commit_transaction()

    def prepare_updates(self, ds):

        commit_key = self.commit_key
        if self.value_format == '8t':
            commit_value = 98
            prepare_value = 99
        else:
            commit_value = b"bbbbb" * 100
            prepare_value = b"ccccc" * 100

        # Set oldest and stable timestamp for the database.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        # Commit some updates to get eviction and history store fired up.
        # Insert a key at timestamp 1.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nsessions * self.nkeys):
            self.session.begin_transaction()
            key = commit_key + ds.key(self.nrows + i)
            cursor.set_key(key)
            cursor.set_value(commit_value)
            self.assertEquals(cursor.insert(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()

        # Call checkpoint.
        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nsessions * self.nkeys):
            self.session.begin_transaction()
            key = commit_key + ds.key(self.nrows + i)
            cursor.set_key(key)
            self.assertEquals(cursor.remove(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()

        # Move the stable timestamp to match the timestamp for the last update.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        hs_writes_start = self.get_stat(stat.conn.cache_write_hs)
        # Have prepared updates in multiple sessions. This should ensure writing prepared updates to
        # the data store. Insert the same key at timestamp 20, but with prepare updates.
        sessions = [0] * self.nsessions
        cursors = [0] * self.nsessions
        for j in range (0, self.nsessions):
            sessions[j] = self.conn.open_session()
            sessions[j].begin_transaction()
            cursors[j] = sessions[j].open_cursor(self.uri)
            # Each session will update many consecutive keys.
            start = (j * self.nkeys)
            end = start + self.nkeys
            for i in range(start, end):
                cursors[j].set_key(commit_key + ds.key(self.nrows + i))
                cursors[j].set_value(prepare_value)
                self.assertEquals(cursors[j].insert(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        hs_writes = self.get_stat(stat.conn.cache_write_hs) - hs_writes_start
        # Assert if not writing anything to the history store.
        self.assertGreaterEqual(hs_writes, 0)

        txn_config = 'read_timestamp=' + self.timestamp_str(5) + ',ignore_prepare=true'
        # Search keys with timestamp 5, ignore_prepare=true and expect the cursor search to return 0 (key found)
        self.search_keys_timestamp_and_ignore(ds, txn_config, commit_value)

        txn_config = 'read_timestamp=' + self.timestamp_str(20) + ',ignore_prepare=true'
        # Search keys with timestamp 20, ignore_prepare=true, expect the cursor to return wiredtiger.WT_NOTFOUND
        self.search_keys_timestamp_and_ignore(ds, txn_config, None)

        prepare_conflict_msg = '/conflict with a prepared update/'
        txn_config = 'read_timestamp=' + self.timestamp_str(20) + ',ignore_prepare=false'
        # Search keys with timestamp 20, ignore_prepare=false and expect the cursor the cursor search to return prepare conflict message
        self.search_keys_timestamp_and_ignore(ds, txn_config, prepare_conflict_msg, True)

        # If commit is True then commit the transactions and simulate a crash which would
        # eventualy rollback transactions.
        if self.commit == True:
            # Commit the prepared_transactions with timestamp 30.
            for j in range (0, self.nsessions):
                sessions[j].commit_transaction(
                    'commit_timestamp=' + self.timestamp_str(30) + ',durable_timestamp=' + self.timestamp_str(30))
            # Move the stable timestamp to match the durable timestamp for prepared updates.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.session.checkpoint()

        # Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the new directory.
        self.conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(self.conn)

        # After simulating a crash, search for the keys inserted. Rollback-to-stable as part of recovery
        # will try restoring values according to the last known stable timestamp.

        # Search keys with timestamp 5, ignore_prepare=true and expect the cursor search to return
        # value committed before prepared update.
        txn_config = 'read_timestamp=' + self.timestamp_str(5) + ',ignore_prepare=false'
        self.search_keys_timestamp_and_ignore(ds, txn_config, commit_value)

        # Search keys with timestamp 20, ignore_prepare=true and expect the cursor search to return
        # WT_NOTFOUND.
        txn_config = 'read_timestamp=' + self.timestamp_str(20) + ',ignore_prepare=true'
        self.search_keys_timestamp_and_ignore(ds, txn_config, None)

        # Search keys with timestamp 20, ignore_prepare=false and expect the cursor search to return WT_NOTFOUND.
        txn_config = 'read_timestamp=' + self.timestamp_str(20) + ',ignore_prepare=false'
        self.search_keys_timestamp_and_ignore(ds, txn_config, None)

        # If commit is true then the commit_tramsactions was called and we will expect prepare_value.
        if self.commit == True:
            txn_config = 'read_timestamp=' + self.timestamp_str(30) + ',ignore_prepare=true'
            # Search keys with timestamp 30, ignore_prepare=true and expect the cursor value to be prepare_value.
            self.search_keys_timestamp_and_ignore(ds, txn_config, prepare_value)
        else:
            # Commit is false and we simulated a crash/restart which would have rolled-back the transactions, therefore we expect the
            # cursor search to return WT_NOTFOUND.
            txn_config = 'read_timestamp=' + self.timestamp_str(30) + ',ignore_prepare=true'
            # Search keys with timestamp 30, ignore_prepare=true and expect the cursor value to return WT_NOTFOUND.
            self.search_keys_timestamp_and_ignore(ds, txn_config, None)

        if self.commit == True:
            txn_config = 'read_timestamp=' + self.timestamp_str(30) + ',ignore_prepare=false'
            # Search keys with timestamp 30, ignore_prepare=false and expect the cursor value to be prepare_value.
            self.search_keys_timestamp_and_ignore(ds, txn_config, prepare_value)
        else:
            # Commit is false and we simulated a crash/restart which would have rolled-back the transactions, therefore we expect the
            # cursor search to return WT_NOTFOUND.
            txn_config = 'read_timestamp=' + self.timestamp_str(30) + ',ignore_prepare=false'
            # Search keys with timestamp 30, ignore_prepare=false and expect the cursor value to return WT_NOTFOUND.
            self.search_keys_timestamp_and_ignore(ds, txn_config, None)

    def test_prepare_hs(self):

        ds = SimpleDataSet(
            self, self.uri, self.nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            bigvalue = 97
        else:
            bigvalue = b"aaaaa" * 100

        # Initially load huge data
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, 10000):
            cursor.set_key(ds.key(self.nrows + i))
            cursor.set_value(bigvalue)
            self.assertEquals(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # We put prepared updates in multiple sessions so that we do not hang
        # because of cache being full with uncommitted updates.
        self.prepare_updates(ds)

if __name__ == '__main__':
    wttest.run()
