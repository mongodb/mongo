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
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs01.py
# Test that update and modify operations are durable across crash and recovery.
# Additionally test that checkpoint inserts content into the history store.
class test_hs01(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=200MB,statistics=(all)'
    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='u')),
        ('row_string', dict(key_format='S', value_format='u'))
    ]
    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, session, uri, value, ds, nrows, timestamp=False):
        cursor = session.open_cursor(uri)
        for i in range(1, nrows):
            if timestamp == True:
                session.begin_transaction()
            cursor.set_key(ds.key(i))
            cursor.set_value(value)
            self.assertEqual(cursor.update(), 0)
            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))
        cursor.close()

    def large_modifies(self, session, uri, offset, ds, nrows, timestamp=False):
        cursor = session.open_cursor(uri)
        for i in range(1, nrows):
            # Unlike inserts and updates, modify operations do not implicitly start/commit a transaction.
            # Hence, we begin/commit transaction manually.
            session.begin_transaction()
            cursor.set_key(ds.key(i))

            # FLCS doesn't allow modify (it doesn't make sense) so just update to 'j' then 'k'.
            if self.value_format == '8t':
                cursor.set_value(106 + offset)
                self.assertEqual(cursor.update(), 0)
            else:
                mods = []
                mod = wiredtiger.Modify('A', offset, 1)
                mods.append(mod)
                self.assertEqual(cursor.modify(mods), 0)

            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))
            else:
                session.commit_transaction()
        cursor.close()

    def durable_check(self, check_value, uri, ds):
        # Simulating recovery.
        newdir = "BACKUP"
        copy_wiredtiger_home(self, '.', newdir, True)
        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        cursor = session.open_cursor(uri, None)

        cursor.next()
        self.assertTrue(check_value == cursor.get_value(),
            "for key " + str(1) + ", expected " + str(check_value) +
            ", got " + str(cursor.get_value()))
        cursor.close()
        session.close()
        conn.close()

    def test_hs(self):
        # Create a small table.
        uri = "table:test_hs01"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            bigvalue = 97
            bigvalue2 = 98
            bigvalue3 = 107
            bigvalue4 = 100
        else:
            bigvalue = b"aaaaa" * 100
            bigvalue2 = b"ccccc" * 100
            bigvalue3 = b"ccccc" * 100
            bigvalue3 = b'AA' + bigvalue3[2:]
            bigvalue4 = b"ddddd" * 100

        # Initially insert a lot of data.
        nrows = 10000
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            cursor.set_value(bigvalue)
            self.assertEqual(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # Scenario: 1
        # Check to see if the history store is working with the old reader.
        # Open session 2.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        # Large updates with session 1.
        self.large_updates(self.session, uri, bigvalue2, ds, nrows)

        # Checkpoint and then assert that the (nrows-1) insertions were moved to history store from data store.
        self.session.checkpoint()
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        self.assertEqual(hs_writes, nrows-1)

        # Check to see the latest updated value after recovery.
        self.durable_check(bigvalue2, uri, ds)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 2
        # Check to see the history store working with modify operations.
        # Open session 2.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        # Apply two modify operations (session1)- replacing the first two letters with 'A'.
        self.large_modifies(self.session, uri, 0, ds, nrows)
        self.large_modifies(self.session, uri, 1, ds, nrows)

        # Checkpoint and then assert if updates (nrows-1) and first large modifies (nrows-1) were moved to history store.
        self.session.checkpoint()
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        # The updates in data store: nrows-1
        # The first modifies in cache: nrows-1
        # The stats was already set at: nrows-1 (previous hs stats)
        # Total: (nrows-1)*3
        self.assertEqual(hs_writes, (nrows-1) * 3)

        # Check to see the modified value after recovery.
        self.durable_check(bigvalue3, uri, ds)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 3
        # Check to see if the history store is working with the old timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.large_updates(self.session, uri, bigvalue4, ds, nrows, timestamp=True)

        self.session.checkpoint()
        # Check if the (nrows-1) modifications were moved to history store from data store.
        # The stats was already set at: (nrows-1)*3 (previous hs stats)
        # Total: (nrows-1)*4
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        self.assertEqual(hs_writes, (nrows-1) * 4)

        # Check to see data can be see only till the stable_timestamp.
        self.durable_check(bigvalue3, uri, ds)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(i + 1))
        # No need to check history store stats here as nothing will be moved.
        self.session.checkpoint()
        # Check that the latest data can be seen.
        self.durable_check(bigvalue4, uri, ds)

if __name__ == '__main__':
    wttest.run()
