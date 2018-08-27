#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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

    def prepare_updates(self, session, uri, value, ds, nrows):
        # Update a large number of records with prepare transactions using
        # multiple sessions. we'll hang if lookaside table isn't doing its
        # thing.
        #
        # If we do all updates in a single session, then hang will be due to
        # uncommitted updates, instead of prepared updates.
        #
        # If we increase nsessions below, then hang will occur.
        nsessions = 1
        sessions = [0] * nsessions
        cursors = [0] * nsessions
        for j in range (0, nsessions):
            sessions[j] = self.conn.open_session()
            sessions[j].begin_transaction("isolation=snapshot")
            cursors[j] = sessions[j].open_cursor(uri)
            # Each session will update many consecutive keys.
            nkeys = 4000
            start = (j * nkeys)
            end = start + nkeys
            for i in range(start, end):
                cursors[j].set_key(ds.key(nrows + i))
                cursors[j].set_value(value)
                self.assertEquals(cursors[j].update(), 0)
            sessions[j].prepare_transaction('prepare_timestamp=' + timestamp_str(2))

        # Close all cursors and sessions.
        for j in range (1, nsessions):
            cursors[j].close()
            sessions[j].close()

    def test_prepare_lookaside(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

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

        # Check to see lookaside working with prepare transactions.
        bigvalue1 = "bbbbb" * 100
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(1))
        self.prepare_updates(self.session, uri, bigvalue1, ds, nrows)

if __name__ == '__main__':
    wttest.run()
