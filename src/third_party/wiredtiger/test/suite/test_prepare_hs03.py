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
from metadata_helper import checkpoint_extent_list_blocks
import wttest
from wtdataset import SimpleDataSet
import os
from wtscenario import make_scenarios
from wiredtiger import stat

# test to ensure salvage, verify & simulating crash are working for prepared transactions.
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
@wttest.skip_for_hook("disagg", "Salvage on disagg tables not yet implemented") # FIXME-WT-14740: Re-enable salvage once implemented.
class test_prepare_hs03(wttest.WiredTigerTestCase):
    # Force a small cache.
    test_name = __qualname__
    conn_config = ('cache_size=50MB,statistics=(fast),'
                   'eviction_dirty_trigger=50,eviction_updates_trigger=50')

    # Create a small table.
    uri = f"table:{test_name}"

    corrupt_values = [
        ('corrupt_table', dict(corrupt=True)),
        ('dont_corrupt_table', dict(corrupt=False))
    ]

    # The impact of corrupting the database file depends on how much of the file is overwritten,
    # depending on what the rest of the test does and expects. A fraction of the table keeps this
    # proportionate however well the values happen to compress; a fraction of zero corrupts a
    # single block. The amount of data that is corrupted by the 'string-row' tests is much larger,
    # as that increases the chance of interactions with, for example, the results of combining
    # timestamp hooks into the test.
    corrupt_fraction = 0.033

    # A dictionary collapses this test's uniform values by more than an order of magnitude, which
    # exercises salvage against a far denser file.
    format_values = [
        ('column', dict(key_format='r', corrupt_fraction=0, dictionary=False)),
        ('string-row', dict(key_format='S', dictionary=False)),
        ('string-row-dictionary', dict(key_format='S', dictionary=True)),
    ]

    value_format='u'

    # Every key checked by this test passes through three values: an untimestamped initial load, a
    # committed update, and a prepared update that is never resolved.
    load_end = 10000
    load_value = b"aaaaa" * 100
    commit_value = b"bbbbb" * 100
    prepare_value = b"ccccc" * 100

    scenarios = make_scenarios(corrupt_values, format_values)

    def value_is_acceptable(self, i, value):
        if value == self.commit_value:
            return True
        # Salvage recovers a key range whose current page was corrupted from an older copy of that
        # range elsewhere in the file, so a key can come back holding the value it had before the
        # committed update. Only the keys the initial load wrote have such an older value, and only
        # a corrupted table can send salvage looking for one. The prepared value is never
        # acceptable: it must not be visible below the prepare timestamp.
        return self.corrupt and i < self.load_end and value == self.load_value

    def corrupt_table(self):
        # Resolve the table against the connection's home: after the simulated crash the connection
        # is open on the copied directory, and a relative name would corrupt the database the test
        # has already finished with rather than the one it is about to salvage.
        tablename=os.path.join(self.home, f"{self.test_name}.wt")
        self.assertEqual(os.path.exists(tablename), True)

        # Leave the checkpoint's extent-list blocks intact. Salvage cannot recover a corrupt extent
        # list, and a later checkpoint that drops this one reads its extent lists: a failed read
        # there is fatal (a WT_PANIC under the default corruption_abort), so corrupting one aborts
        # the test whenever the file layout happens to place an extent-list block in the range below.
        protect = sorted(checkpoint_extent_list_blocks(self.session, 'file:' + self.test_name + '.wt'))
        start = 1024
        # Overwrite at least one block however small the table turned out to be.
        size = max(4096, int(os.path.getsize(tablename) * self.corrupt_fraction))
        pattern = b'Bad!'
        data = (pattern * (size // len(pattern) + 1))[:size]
        end = start + len(data)
        with open(tablename, 'r+b') as f:
            pos = start
            for b_off, b_size in protect:
                b_end = b_off + b_size
                if b_end <= pos or b_off >= end:
                    continue
                if b_off > pos:
                    f.seek(pos)
                    f.write(data[pos - start:b_off - start])
                pos = max(pos, b_end)
            if pos < end:
                f.seek(pos)
                f.write(data[pos - start:])

    def corrupt_salvage_verify(self):
        # An exclusive handle operation can fail if there is dirty data in the cache, closing the
        # open handles before acquiring an exclusive handle will return EBUSY. A checkpoint should
        # clear the dirty data, but eviction can re-dirty the cache between the checkpoint and the
        # open attempt, we have to loop.
        self.session.checkpoint()
        if self.corrupt:
            self.corrupt_table()
        while True:
            if not self.raisesBusy(lambda: self.session.salvage(self.uri, "force")):
                break
            self.session.checkpoint()
        while True:
            if not self.raisesBusy(lambda: self.session.verify(self.uri, None)):
                break
            self.session.checkpoint()

    def check_data(self, ds, message, nkeys_end, nrows, timestamp):
        # Search for the keys inserted with commit timestamp
        cursor = self.session.open_cursor(self.uri)
        self.pr('check_data: {}'.format(message))
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(timestamp))
        nkeys_checked = 0
        nkeys_stale = 0
        unexpected = []
        for i in range(1, nkeys_end):
            key = nrows + i
            cursor.set_key(ds.key(key))
            # It is not guaranteed that salvage recovers all the data in the table, so count the
            # keys a search actually finds rather than requiring all of them.
            if cursor.search() != 0:
                self.pr('Key {} not found'.format(key))
                continue
            nkeys_checked += 1
            value = cursor.get_value()
            # Report the value a key came back with, not just how many keys disagreed. Count the
            # keys salvage rebuilt from an older image separately: accepting them is necessary but
            # it is also the tolerance most likely to hide a real regression, so it must be
            # visible rather than silent.
            if value == self.load_value:
                nkeys_stale += 1
            if not self.value_is_acceptable(i, value):
                unexpected.append((key, value[:16]))
        self.pr("nkeys_checked = {}, nkeys_stale = {}, unexpected = {}".format(
            nkeys_checked, nkeys_stale, len(unexpected)))
        self.assertEqual(unexpected, [], 'unexpected values: {}'.format(unexpected[:10]))
        # Bound how much salvage is allowed to lose. Counting only the keys a search finds says
        # nothing about how many it found, so without a floor this passes having recovered one key.
        nkeys_expected = nkeys_end - 1
        if self.corrupt:
            # A key whose only copy was in the corrupted range is gone for good, but the corruption
            # covers a small fraction of the file, so losing most of the table means salvage
            # discarded data it could have kept.
            self.assertGreaterEqual(nkeys_checked, nkeys_expected // 2,
                'salvage recovered {} of {} keys'.format(nkeys_checked, nkeys_expected))
        else:
            self.assertEqual(nkeys_checked, nkeys_expected)
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
            cursor.set_value(self.commit_value)
            self.assertEqual(cursor.insert(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp_early))
        cursor.close()

        # Set the stable/oldest timestamps.
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
                cursors[j].set_value(self.prepare_value)
                self.assertEqual(cursors[j].insert(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + self.timestamp_str(timestamp_later))

        hs_writes = self.get_stat(stat.conn.cache_write_hs) - hs_writes_start

        # Assert if not writing anything to the history store.
        self.assertGreaterEqual(hs_writes, 0)

        self.check_data(ds, "(step 1)", nsessions * nkeys, nrows, timestamp_middle)

        # Test if we can read prepared updates from the history store. Hold the read transaction
        # open across the rollback of the prepared updates below.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(timestamp_middle))
        for i in range(1, nsessions * nkeys):
            cursor.set_key(ds.key(nrows + i))
            if cursor.search() != 0:
                continue
            # A read below the prepare timestamp sees the committed value, never the prepared one.
            value = cursor.get_value()
            self.assertTrue(self.value_is_acceptable(i, value),
                'key {} holds unexpected value {}'.format(nrows + i, value[:16]))
        cursor.close()

        # Close all sessions (and cursors), this will cause prepared updates to be rolled back.
        for j in range (0, nsessions):
            sessions[j].close()

        self.session.commit_transaction()

        self.check_data(ds, "(step 2)", nsessions * nkeys, nrows, timestamp_later)

        # Corrupt the table, call salvage to recover data from the corrupted table and call verify
        self.corrupt_salvage_verify()

        # Finally, search for the keys inserted with commit timestamp
        self.check_data(ds, "(step 3)", nsessions * nkeys, nrows, timestamp_later)

        self.session.checkpoint()

        # Simulate a crash by copying to a new directory(RESTART).
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the new directory.
        self.conn = self.setUpConnectionOpen("RESTART")
        self.session = self.setUpSessionOpen(self.conn)

        self.check_data(ds, "(step 4)", nsessions * nkeys, nrows, timestamp_later)

        # After simulating a crash, corrupt the table, call salvage to recover data from the
        # corrupted table and call verify
        self.corrupt_salvage_verify()

        self.check_data(ds, "(step 5)", nsessions * nkeys, nrows, timestamp_later)

    def test_prepare_hs(self):
        nrows = 100
        ds = SimpleDataSet(
            self, self.uri, nrows, key_format=self.key_format, value_format=self.value_format,
            config='dictionary=1' if self.dictionary else '')
        ds.populate()

        # Initially load huge data
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.load_end):
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(self.load_value)
            self.assertEqual(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # We put prepared updates in multiple sessions so that we do not hang
        # because of cache being full with uncommitted updates.
        nsessions = 3
        nkeys = 4000
        self.prepare_updates(ds, nrows, nsessions, nkeys)
