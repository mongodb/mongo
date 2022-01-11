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

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs27.py
# Test that variable-length column store doesn't RLE-compact adjacent data with heterogeneous
# timestamps.
#
# (The concern doesn't exist for row stores, so while this test could be run for row stores there's
# little benefit to doing so; thus, no row-store scenarios are generated.)
#
# This works by writing the same value to adjacent keys at different times, evicting them, and
# making sure they read back correctly. (The eviction is necessary to go through the RLE code.)
class test_hs27(wttest.WiredTigerTestCase):
    conn_config = ''
    session_config = 'isolation=snapshot'

    nrows = 100
    value_1 = 'a' * 119
    value_2 = 'd' * 119

    # Configure the number of different timestamps to write at.
    ntimes_values = [
        ('two', dict(ntimes=2)),
        ('three', dict(ntimes=3)),
        ('ten', dict(ntimes=10)),
    ]

    # Configure the number of keys to write at each timestamp.
    nkeys_values = [
        ('one', dict(nkeys=1)),
        ('two', dict(nkeys=2)),
        ('three', dict(nkeys=3)),
    ]

    # Configure whether the keys should be initialized first.
    doinit_values = [
        ('init', dict(doinit=True)),
        ('noinit', dict(doinit=False)),
    ]

    # Configure whether the timestamp groups of keys are written forwards or backwards.
    group_forward_values = [
        ('forward', dict(group_forward=True)),
        ('backward', dict(group_forward=False)),
    ]

    # Configure whether the keys within a timestamp group are written forwards or backwards.
    keys_forward_values = [
        ('forward', dict(keys_forward=True)),
        ('backward', dict(keys_forward=False)),
    ]

    scenarios = make_scenarios(ntimes_values, nkeys_values, doinit_values,
        group_forward_values, keys_forward_values)

    # Get the m'th writer timestamp.
    def get_writetime(self, m):
        return 40 + 2 * m

    # Get the m'th reader timestamp.
    def get_readtime(self, m):
        return 41 + 2 * m

    # Get the k'th key for the m'th timestamp.
    def get_key(self, k, m):
        if self.group_forward:
            ts_offset = m * self.nkeys
        else:
            ts_offset = (self.ntimes - m - 1) * self.nkeys
        if self.keys_forward:
            key_offset = k
        else:
            key_offset = self.nkeys - k - 1
        return 71 + ts_offset + key_offset

    # Return the key number k and timestamp number m for a key (inverse of get_key).
    def invert_key(self, key):
        if key < 71 or key >= 71 + self.ntimes * self.nkeys:
            return None
        key -= 71
        if self.group_forward:
            m = key // self.nkeys
        else:
            m = self.ntimes - (key // self.nkeys) - 1
        if self.keys_forward:
            k = key % self.nkeys
        else:
            k = self.nkeys - (key % self.nkeys) - 1
        return (k, m)

    # Return the timestamp number k for a timestamp (inverse of get_read/writetime).
    # Return -1 or self.ntimes for other timestamps as appropriate, for comparison.
    def invert_timestamp(self, ts, beforeval, afterval):
        if ts < 40:
            return beforeval
        if ts >= 40 + self.ntimes * 2:
            return afterval
        ts -= 40
        m = ts // 2
        return m

    # Figure if we should see value_2 for a given key and read time.
    def expect_value_2(self, key, readtime):
        # Get the key number and timestamp number for the key.
        km = self.invert_key(key)
        if km is None:
                return False
        (k, m) = km

        # Get the timestamp number associated with readtime.
        m2 = self.invert_timestamp(readtime, -1, self.ntimes)

        # We should see value_2 if we are at or after the time it was written.
        return m2 >= m

    # Check each value explicitly to make sure it's what we meant to see.
    # Don't bother checking the background values.
    def check1(self, session, uri, ds, readtime, make_own_txn):
        if make_own_txn:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(readtime))
        cursor = session.open_cursor(uri)
        for k in range(0, self.nkeys):
            for m in range(0, self.ntimes):
                key = self.get_key(k, m)
                if self.expect_value_2(key, readtime):
                    self.assertEqual(cursor[ds.key(key)], self.value_2)
                elif self.doinit:
                    self.assertEqual(cursor[ds.key(key)], self.value_1)
                else:
                    self.assertRaisesException(KeyError, lambda: cursor[ds.key(key)])
        if make_own_txn:
            session.rollback_transaction()
        cursor.close()

    # Scan through the whole table and make sure it's what we expect.
    def check2(self, session, uri, readtime, make_own_txn):
        if make_own_txn:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(readtime))
        cursor = session.open_cursor(uri)
        count = 0
        for (key, valueseen) in cursor:
            val = self.value_2 if self.expect_value_2(key, readtime) else self.value_1
            self.assertEqual(valueseen, val)
            count += 1
        if self.doinit:
            # The table was initialized; should always see all rows.
            self.assertEqual(count, self.nrows)
        else:
            # The table was not initialized; should see only what we wrote by this time.
            # (If m is 0 we should see one batch of keys.)
            m = self.invert_timestamp(readtime, -1, self.ntimes - 1)
            self.assertEqual(count, self.nkeys * (m + 1))
        if make_own_txn:
            session.rollback_transaction()
        cursor.close()

    # Scan through the whole table backward too, since cursor_prev isn't adequately tested.
    def check3(self, session, uri, readtime, make_own_txn):
        if make_own_txn:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(readtime))
        cursor = session.open_cursor(uri)
        count = 0
        while cursor.prev() == 0:
            key = cursor.get_key()
            valueseen = cursor[key]
            val = self.value_2 if self.expect_value_2(key, readtime) else self.value_1
            self.assertEqual(valueseen, val)
            count += 1
        if self.doinit:
            # The table was initialized; should always see all rows.
            self.assertEqual(count, self.nrows)
        else:
            # The table was not initialized; should see only what we wrote by this time.
            # (If m is 0 we should see one batch of keys.)
            m = self.invert_timestamp(readtime, -1, self.ntimes - 1)
            self.assertEqual(count, self.nkeys * (m + 1))
        if make_own_txn:
            session.rollback_transaction()
        cursor.close()

    # Do all three checks.
    def check(self, session, uri, ds, readtime, make_own_txn=True):
        self.check1(session, uri, ds, readtime, make_own_txn)
        self.check2(session, uri, readtime, make_own_txn)
        self.check3(session, uri, readtime, make_own_txn)

    # Scan through the whole table at each relevant timestamp and make sure it's what we expect.
    def checkall(self, session, uri, ds):
        self.check(session, uri, ds, 2)
        for m in range(0, self.ntimes):
            self.check(session, uri, ds, self.get_readtime(m))
        self.check(session, uri, ds, 100)

    # Write self.nrows records at timestamp 1, using self.value_1 as the value.
    def initialize(self, uri, ds):
        session = self.session
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[ds.key(i)] = self.value_1
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        cursor.close()

    # Do the m'th update.
    def update(self, uri, ds, m):
        writetime = self.get_writetime(m)

        session = self.session
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        if self.keys_forward:
            for k in range(0, self.nkeys):
                key = self.get_key(k, m)
                cursor[ds.key(key)] = self.value_2
        else:
            for k in range(self.nkeys - 1, -1, -1):
                key = self.get_key(k, m)
                cursor[ds.key(key)] = self.value_2
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(writetime))
        cursor.close()

    # Do all the updates.
    def updateall(self, uri, ds):
        if self.group_forward:
            for m in range(0, self.ntimes):
                self.update(uri, ds, m)
        else:
            for m in range(self.ntimes - 1, -1, -1):
                self.update(uri, ds, m)

    def test_hs(self):

        # Create a file that contains active history (content newer than the oldest timestamp).
        table_uri = 'table:hs27'
        ds = SimpleDataSet(
            self, table_uri, 0, key_format='r', value_format='S', config='log=(enabled=false)')
        ds.populate()
        self.session.checkpoint()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write the initial values, if requested.
        if self.doinit:
            self.initialize(ds.uri, ds)

        # Create a long running read transaction in a separate session.
        session_read = self.conn.open_session()
        session_read.begin_transaction('read_timestamp=' + self.timestamp_str(2))

        # Check that the initial writes (at timestamp 1) are seen (at timestamp 2).
        self.check(session_read, ds.uri, ds, 2, make_own_txn=False)

        # Write more values at assorted timestamps.
        self.updateall(ds.uri, ds)

        # Check that the new updates are appropriately visible.
        self.checkall(self.session, ds.uri, ds)

        # Now forcibly evict, so that all the pages are RLE-encoded and then read back in.
        # There doesn't seem to be any way to just forcibly evict an entire table, so what
        # I'm going to do is assume that what we care about is evicting the updates (the
        # initial values are not so interesting) and they are on a maximum of two pages,
        # so we can evict the first and last key. If this evicts the same page twice, it
        # won't really hurt anything. (This also avoids having to worry about whether we
        # wrote initial values or not.)

        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        firstkey = self.get_key(0, 0)
        lastkey = self.get_key(self.nkeys - 1, self.ntimes - 1)
        for k in [firstkey, lastkey]:
            # Search the key to evict it.
            v = evict_cursor[ds.key(k)]
            self.assertEqual(v, self.value_2)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Check that the long-running read transaction still reads the correct data.
        self.check(session_read, ds.uri, ds, 2, make_own_txn=False)

        # Check that our main session reads the correct data.
        self.checkall(self.session, ds.uri, ds)

        # Drop the long running read transaction.
        session_read.rollback_transaction()

        # Check that our main session can still read the latest data.
        self.check(self.session, ds.uri, ds, 100)

if __name__ == '__main__':
    wttest.run()
