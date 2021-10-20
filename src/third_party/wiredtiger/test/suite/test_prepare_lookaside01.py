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

def timestamp_str(t):
    return '%x' % t

# test_prepare_lookaside01.py
# test to ensure lookaside eviction is working for prepared transactions.
class test_prepare_lookaside01(wttest.WiredTigerTestCase):
    # Force a small cache.
    def conn_config(self):
        return 'cache_size=50MB'

    def prepare_updates(self, uri, ds, nrows, nsessions, nkeys):
        # Update a large number of records in their individual transactions.
        # This will force eviction and start lookaside eviction of committed
        # updates.
        #
        # Follow this by updating a number of records in prepared transactions
        # under multiple sessions. We'll hang if lookaside table isn't doing its
        # thing. If we do all updates in a single session, then hang will be due
        # to uncommitted updates, instead of prepared updates.
        #
        # Do another set of updates in that many transactions. This forces the
        # pages that have been evicted to lookaside to be re-read and brought in
        # memory. Hence testing if we can read prepared updates from lookaside.

        # Start with setting a stable timestamp to pin history in cache
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(1))

        # Commit some updates to get eviction and lookaside fired up
        bigvalue1 = "bbbbb" * 100
        cursor = self.session.open_cursor(uri)
        for i in range(1, nsessions * nkeys):
            self.session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue1)
            self.assertEquals(cursor.update(), 0)
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(i))

        # Have prepared updates in multiple sessions. This should ensure writing
        # prepared updates to the lookaside
        sessions = [0] * nsessions
        cursors = [0] * nsessions
        bigvalue2 = "ccccc" * 100
        for j in range (0, nsessions):
            sessions[j] = self.conn.open_session()
            sessions[j].begin_transaction("isolation=snapshot")
            cursors[j] = sessions[j].open_cursor(uri)
            # Each session will update many consecutive keys.
            start = (j * nkeys)
            end = start + nkeys
            for i in range(start, end):
                cursors[j].set_key(ds.key(nrows + i))
                cursors[j].set_value(bigvalue2)
                self.assertEquals(cursors[j].update(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + timestamp_str(2))

        # Commit more regular updates. To do this, the pages that were just
        # evicted need to be read back. This ensures reading prepared updates
        # from the lookaside
        bigvalue3 = "ddddd" * 100
        cursor = self.session.open_cursor(uri)
        for i in range(1, nsessions * nkeys):
            self.session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue3)
            self.assertEquals(cursor.update(), 0)
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(i + 3))
        cursor.close()

        # Close all cursors and sessions, this will cause prepared updates to be
        # rollback-ed
        for j in range (0, nsessions):
            cursors[j].close()
            sessions[j].close()

    def test_prepare_lookaside(self):
        # Create a small table.
        uri = "table:test_prepare_lookaside01"
        nrows = 100
        ds = SimpleDataSet(self, uri, nrows, key_format="S", value_format='u')
        ds.populate()
        bigvalue = "aaaaa" * 100

        # Initially load huge data
        cursor = self.session.open_cursor(uri)
        for i in range(1, 10000):
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(bigvalue)
            self.assertEquals(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # Check if lookaside is working properly with prepare transactions.
        # We put prepared updates in multiple sessions so that we do not hang
        # because of cache being full with uncommitted updates.
        nsessions = 3
        nkeys = 4000
        self.prepare_updates(uri, ds, nrows, nsessions, nkeys)

if __name__ == '__main__':
    wttest.run()
