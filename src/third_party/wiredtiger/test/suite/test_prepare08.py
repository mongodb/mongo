#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

import fnmatch, os, shutil, time
from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet

def timestamp_str(t):
    return '%x' % t

# test_prepare07.py
# Test to ensure prepared tombstones are properly aborted even when they are written
# to the data store.
class test_prepare08(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=2MB,eviction_updates_trigger=95,eviction_updates_target=80'

    def updates(self, ds, uri, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('isolation=snapshot')
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            cursor.set_value(value)
            self.assertEquals(cursor.insert(), 0)
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(ts))
        cursor.close()

    def check(self, ds, uri, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('ignore_prepare=true,read_timestamp=' + timestamp_str(ts))
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            self.assertEquals(cursor.search(), 0)
            self.assertEquals(cursor.get_value(),value)
        self.session.commit_transaction()
        cursor.close()

    def test_prepare(self):
        # Create a small table.
        uri = "table:test"
        nrows = 1000
        ds = SimpleDataSet(self, uri, 0, key_format="S", value_format='u')
        ds.populate()

        value_a = b"aaaaa" * 100
        value_b = b"bbbbb" * 100

        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(10))

        # Initially load huge data
        self.updates(ds, uri, nrows, value_a, 20)
        # Add some more updates
        self.updates(ds, uri, nrows, value_b, 30)

        # Checkpoint
        self.session.checkpoint()

        # Remove the updates from a prepare session and and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri)
        session_p.begin_transaction('isolation=snapshot')
        for i in range(1, nrows):
            cursor_p.set_key(ds.key(i))
            self.assertEquals(cursor_p.remove(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + timestamp_str(40))

        self.check(ds, uri, nrows, value_a, 20)
        self.check(ds, uri, nrows, value_b, 50)

        #rollback the prepared session
        session_p.rollback_transaction()

        self.check(ds, uri, nrows, value_a, 20)
        self.check(ds, uri, nrows, value_b, 50)

        # close sessions.
        cursor_p.close()
        session_p.close()
        self.session.close()

if __name__ == '__main__':
    wttest.run()
