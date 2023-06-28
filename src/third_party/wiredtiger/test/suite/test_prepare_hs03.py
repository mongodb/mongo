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
# [TEST_TAGS]
# salvage:prepare
# verify:prepare
# [END_TAGS]

from helper import copy_wiredtiger_home
import wttest
from wtdataset import SimpleDataSet
import os
from wtscenario import make_scenarios
from wiredtiger import stat

# test_prepare_hs03.py
# test to ensure salvage, verify & simulating crash are working for prepared transactions.
class test_prepare_hs03(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = ('cache_size=50MB,statistics=(fast),'
                   'eviction_dirty_trigger=50,eviction_updates_trigger=50')

    # Create a small table.
    uri = "table:test_prepare_hs03"

    corrupt_values = [
        ('corrupt_table', dict(corrupt=True)),
        ('dont_corrupt_table', dict(corrupt=False))
    ]

    # The impact of corrupting the database file can depend on the number of bytes overwritten,
    # depending on what the rest of the test does and expects. The amount of data that
    # is corrupted by the 'string-row' test is much larger, as that increases the chance of interactions
    # with, for example, the results of combining timestamp hooks into the test.
    format_values = [
        ('column', dict(key_format='r', value_format='u', data_to_corrupt_with='Bad!' * 1024)),
        ('column-fix', dict(key_format='r', value_format='8t', data_to_corrupt_with='Bad!' * 1024)),
        ('string-row', dict(key_format='S', value_format='u', data_to_corrupt_with='Bad!' * 100 * 1024)),
    ]

    scenarios = make_scenarios(corrupt_values, format_values)

    def corrupt_table(self, data_to_corrupt_with):
        tablename="test_prepare_hs03.wt"
        self.assertEquals(os.path.exists(tablename), True)

        # This code will overwrite part of the table with 'bad' data, corrupting the table in the process.
        # The impact of this overwriting can depend on the number of bytes overwritten, depending on what the
        # rest of the test does and expects.
        with open(tablename, 'r+') as tablepointer:
            tablepointer.seek(1024)
            tablepointer.write(data_to_corrupt_with)

    def corrupt_salvage_verify(self):
        # An exclusive handle operation can fail if there is dirty data in the cache, closing the
        # open handles before acquiring an exclusive handle will return EBUSY. A checkpoint should
        # clear the dirty data, but eviction can re-dirty the cache between the checkpoint and the
        # open attempt, we have to loop.
        self.session.checkpoint()
        if self.corrupt == True:
            self.corrupt_table(self.data_to_corrupt_with)
        while True:
            if not self.raisesBusy(lambda: self.session.salvage(self.uri, "force")):
                break
            self.session.checkpoint()
        while True:
            if not self.raisesBusy(lambda: self.session.verify(self.uri, None)):
                break
            self.session.checkpoint()

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def check_data(self, ds, message, nkeys, nrows, timestamp, expected_value):
        # Search for the keys inserted with commit timestamp
        cursor = self.session.open_cursor(self.uri)
        self.pr('check_data: {}'.format(message))
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(timestamp))
        correct_values = 0
        for i in range(1, nkeys):
            key = nrows + i
            if cursor.set_key(ds.key(key)) != 0:
                # The search should pass
                search_result = cursor.search()
                if search_result == 0:
                    # Correctness Test - expected_value should be visible
                    if cursor.get_value() == expected_value:
                        correct_values += 1
                else:
                    self.pr('Key {} not found'.format(key))
        # A range() is exclusive of the upper bound, so the number of keys actually checked is one less than nkeys.
        nkeys_checked = nkeys - 1
        self.pr("nkeys_checked = {}, correct_values = {}".format(nkeys_checked, correct_values))
        self.assertEquals(nkeys_checked, correct_values)
        cursor.close()
        self.session.commit_transaction()

    def get_timestamps(self):
        timestamp = self.getTimestamp()
        if timestamp:
            # Get the next available timestamp values to avoid clashing with timestamp hooks
            return timestamp.get_incr(), timestamp.get_incr(), timestamp.get_incr()
        else:
            # Return three timestamp values that increase in order
            return 1, 2, 3

    def prepare_updates(self, ds, nrows, nsessions, nkeys):
        if self.value_format == '8t':
            commit_value = 98
            prepare_value = 99
        else:
            commit_value = b"bbbbb" * 100
            prepare_value = b"ccccc" * 100

        # Three timestamps are required for this test, and they must be in the sequence 'early', 'middle' & 'later'.
        timestamps = self.get_timestamps()
        timestamp_early = timestamps[0]
        timestamp_middle = timestamps[1]
        timestamp_later = timestamps[2]
        self.pr("Timestamps: timestamp_early={}, timestamp_middle={}, timestamp_later={}".
                format(timestamp_early, timestamp_middle, timestamp_later))

        # Commit some updates to get eviction and history store fired up
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, nsessions * nkeys):
            self.session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(commit_value)
            self.assertEquals(cursor.insert(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp_early))
        cursor.close()

        # Set the stable/oldest timstamps.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(timestamp_early))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(timestamp_early))

        # Corrupt the table, call salvage to recover data from the corrupted table and call verify
        self.corrupt_salvage_verify()

        hs_writes_start = self.get_stat(stat.conn.cache_write_hs)

        # Have prepared updates in multiple sessions. This should ensure writing
        # prepared updates to the history store
        sessions = [0] * nsessions
        cursors = [0] * nsessions
        for j in range (0, nsessions):
            sessions[j] = self.conn.open_session()
            sessions[j].begin_transaction()
            cursors[j] = sessions[j].open_cursor(self.uri)
            # Each session will update many consecutive keys.
            start = (j * nkeys)
            end = start + nkeys
            for i in range(start, end):
                cursors[j].set_key(ds.key(nrows + i))
                cursors[j].set_value(prepare_value)
                self.assertEquals(cursors[j].insert(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + self.timestamp_str(timestamp_later))

        hs_writes = self.get_stat(stat.conn.cache_write_hs) - hs_writes_start

        # Assert if not writing anything to the history store.
        self.assertGreaterEqual(hs_writes, 0)

        self.check_data(ds, "(step 1)", nkeys, nrows, timestamp_middle, commit_value)

        # Test if we can read prepared updates from the history store.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(timestamp_middle))
        for i in range(1, nsessions * nkeys):
            cursor.set_key(ds.key(nrows + i))
            # The search should pass.
            self.assertEqual(cursor.search(), 0)
            # Correctness Test - commit_value should be visible
            self.assertEquals(cursor.get_value(), commit_value)
            # Correctness Test - prepare_value should NOT be visible
            self.assertNotEquals(cursor.get_value(), prepare_value)
        cursor.close()

        # Close all sessions (and cursors), this will cause prepared updates to be rolled back.
        for j in range (0, nsessions):
            sessions[j].close()

        self.session.commit_transaction()

        self.check_data(ds, "(step 2)", nkeys, nrows, timestamp_later, commit_value)

        # Corrupt the table, call salvage to recover data from the corrupted table and call verify
        self.corrupt_salvage_verify()

        # Finally, search for the keys inserted with commit timestamp
        self.check_data(ds, "(step 3)", nkeys, nrows, timestamp_later, commit_value)

        self.session.checkpoint()

        # Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the new directory.
        self.conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(self.conn)

        self.check_data(ds, "(step 4)", nkeys, nrows, timestamp_later, commit_value)

        # After simulating a crash, corrupt the table, call salvage to recover data from the
        # corrupted table and call verify
        self.corrupt_salvage_verify()

    def test_prepare_hs(self):
        nrows = 100
        ds = SimpleDataSet(
            self, self.uri, nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            bigvalue = 97
        else:
            bigvalue = b"aaaaa" * 100

        # Initially load huge data
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, 10000):
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue)
            self.assertEquals(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # We put prepared updates in multiple sessions so that we do not hang
        # because of cache being full with uncommitted updates.
        nsessions = 3
        nkeys = 4000
        self.prepare_updates(ds, nrows, nsessions, nkeys)


if __name__ == '__main__':
    wttest.run()
