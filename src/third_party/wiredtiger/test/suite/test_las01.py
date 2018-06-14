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

# test_las01.py
# Smoke tests to ensure lookaside tables are working.
class test_las01(wttest.WiredTigerTestCase):
    # Force a small cache.
    def conn_config(self):
        return 'cache_size=50MB'

    def large_updates(self, session, uri, value, ds, nrows, timestamp=False):
        # Update a large number of records, we'll hang if the lookaside table
        # isn't doing its thing.
        cursor = session.open_cursor(uri)
        for i in range(1, 10000):
            if timestamp == True:
                session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(value)
            self.assertEquals(cursor.update(), 0)
            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + timestamp_str(i + 1))
        cursor.close()

    def large_modifies(self, session, uri, offset, ds, nrows, timestamp=False):
        # Modify a large number of records, we'll hang if the lookaside table
        # isn't doing its thing.
        cursor = session.open_cursor(uri)
        for i in range(1, 10000):
            if timestamp == True:
                session.begin_transaction()
            cursor.set_key(ds.key(nrows + i))
            mods = []
            mod = wiredtiger.Modify('A', offset, 1)
            mods.append(mod)

            self.assertEqual(cursor.modify(mods), 0)
            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + timestamp_str(i + 1))
        cursor.close()

    def durable_check(self, check_value, uri, ds, nrows):
        # Checkpoint and backup so as to simulate recovery
        self.session.checkpoint()
        newdir = "BACKUP"
        copy_wiredtiger_home('.', newdir, True)

        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        cursor = session.open_cursor(uri, None)
        # Skip the initial rows, which were not updated
        for i in range(0, nrows+1):
            self.assertEquals(cursor.next(), 0)
        if (check_value != cursor.get_value()):
            print "Check value : " + str(check_value)
            print "value : " + str(cursor.get_value())
        self.assertTrue(check_value == cursor.get_value())
        cursor.close()
        session.close()
        conn.close()

    def test_las(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        # Create a small table.
        uri = "table:test_las01"
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

        # Scenario: 1
        # Check to see LAS working with old snapshot
        bigvalue1 = "bbbbb" * 100
        self.session.snapshot("name=xxx")
        # Update the values in different session after snapshot
        self.large_updates(self.session, uri, bigvalue1, ds, nrows)
        # Check to see the value after recovery
        self.durable_check(bigvalue1, uri, ds, nrows)
        self.session.snapshot("drop=(all)")

        # Scenario: 2
        # Check to see LAS working with old reader
        bigvalue2 = "ccccc" * 100
        session2 = self.conn.open_session()
        session2.begin_transaction('isolation=snapshot')
        self.large_updates(self.session, uri, bigvalue2, ds, nrows)
        # Check to see the value after recovery
        self.durable_check(bigvalue2, uri, ds, nrows)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 3
        # Check to see LAS working with modify operations
        bigvalue3 = "ccccc" * 100
        bigvalue3 = 'AA' + bigvalue3[2:]
        session2 = self.conn.open_session()
        session2.begin_transaction('isolation=snapshot')
        # Apply two modify operations - replacing the first two items with 'A'
        self.large_modifies(self.session, uri, 0, ds, nrows)
        self.large_modifies(self.session, uri, 1, ds, nrows)
        # Check to see the value after recovery
        self.durable_check(bigvalue3, uri, ds, nrows)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 4
        # Check to see LAS working with old timestamp
        bigvalue4 = "ddddd" * 100
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(1))
        self.large_updates(self.session, uri, bigvalue4, ds, nrows, timestamp=True)
        # Check to see data can be see only till the stable_timestamp
        self.durable_check(bigvalue3, uri, ds, nrows)

        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(i + 1))
        # Check to see latest data can be seen
        self.durable_check(bigvalue4, uri, ds, nrows)

if __name__ == '__main__':
    wttest.run()
