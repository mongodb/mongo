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

import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_prepare_hs01.py
# test to ensure history store eviction is working for prepared transactions.
class test_prepare_hs01(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=50MB,eviction_updates_trigger=95,eviction_updates_target=80'

    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='u')),
    ]

    scenarios = make_scenarios(format_values)

    def check(self, uri, ds, nrows, nsessions, nkeys, read_ts, expected_value, not_expected_value):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        for i in range(1, nsessions * nkeys):
            cursor.set_key(ds.key(nrows + i))
            self.assertEquals(cursor.search(), 0)
            # Correctness Test - commit_value should be visible
            self.assertEquals(cursor.get_value(), expected_value)
            # Correctness Test - prepare_value should NOT be visible
            self.assertNotEquals(cursor.get_value(), not_expected_value)
        cursor.close()
        self.session.commit_transaction()

    def prepare_updates(self, uri, ds, nrows, nsessions, nkeys):
        # Update a large number of records in their individual transactions.
        # This will force eviction and start history store eviction of committed
        # updates.
        #
        # Follow this by updating a number of records in prepared transactions
        # under multiple sessions. We'll hang if the history store table isn't doing its
        # thing. If we do all updates in a single session, then hang will be due
        # to uncommitted updates, instead of prepared updates.
        #
        # Do another set of updates in that many transactions. This forces the
        # pages that have been evicted to the history store to be re-read and brought in
        # memory. Hence testing if we can read prepared updates from the history store.

        # Start with setting a stable timestamp to pin history in cache
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        if self.value_format == '8t':
            bigvalue1 = 98
            bigvalue2 = 99
        else:
            bigvalue1 = b"bbbbb" * 100
            bigvalue2 = b"ccccc" * 100

        # Commit some updates to get eviction and history store fired up
        cursor = self.session.open_cursor(uri)
        for i in range(1, nsessions * nkeys):
            self.session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue1)
            self.assertEquals(cursor.insert(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        # Have prepared updates in multiple sessions. This should ensure writing
        # prepared updates to the history store
        sessions = [0] * nsessions
        cursors = [0] * nsessions
        for j in range (0, nsessions):
            sessions[j] = self.conn.open_session()
            sessions[j].begin_transaction()
            cursors[j] = sessions[j].open_cursor(uri)
            # Each session will update many consecutive keys.
            start = (j * nkeys)
            end = start + nkeys
            for i in range(start, end):
                cursors[j].set_key(ds.key(nrows + i))
                cursors[j].set_value(bigvalue2)
                self.assertEquals(cursors[j].insert(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + self.timestamp_str(2))

        # Re-read the original versions of all the data. This ensures reading
        # original versions from the history store
        self.check(uri, ds, nrows, nsessions, nkeys, 1, bigvalue1, bigvalue2)

        # Close all cursors and sessions, this will cause prepared updates to be
        # rollback-ed
        for j in range (0, nsessions):
            cursors[j].close()
            sessions[j].close()

        # Re-read the original versions of all the data. This ensures reading
        # original versions from the data store as the prepared updates are
        # aborted
        self.check(uri, ds, nrows, nsessions, nkeys, 2, bigvalue1, bigvalue2)

    def test_prepare_hs(self):
        # Create a small table.
        uri = "table:test_prepare_hs01"
        nrows = 100
        ds = SimpleDataSet(
            self, uri, nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            bigvalue = 97
        else:
            bigvalue = b"aaaaa" * 100

        # Initially load huge data
        cursor = self.session.open_cursor(uri)
        for i in range(1, 10000):
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue)
            self.assertEquals(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # Check if the history store is working properly with prepare transactions.
        # We put prepared updates in multiple sessions so that we do not hang
        # because of cache being full with uncommitted updates.
        nsessions = 3
        nkeys = 4000
        self.prepare_updates(uri, ds, nrows, nsessions, nkeys)

if __name__ == '__main__':
    wttest.run()
