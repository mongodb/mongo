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

import fnmatch, os, shutil, time
from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_prepare07.py
# Test to ensure prepared updates older than oldest timestamp are not visible.
# this test is mainly to ensure there is no gap in txn_visible_all when active
# oldest transaction is a prepared transaction, and the oldest timestamp advanced
# past the prepared timestamp.
class test_prepare07(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=50MB'

    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='u')),
    ]

    scenarios = make_scenarios(format_values)

    def older_prepare_updates(self, uri, ds, nrows, value_a, value_b):
        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(100))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))

        # Commit some updates.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor.set_key(ds.key(nrows + 1))
        cursor.set_value(value_b)
        self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(110))
        self.session.begin_transaction()
        cursor.set_key(ds.key(nrows + 2))
        cursor.set_value(value_b)
        self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(120))

        # Prepare a transaction and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri)
        session_p.begin_transaction()
        cursor_p.set_key(ds.key(nrows + 3))
        cursor_p.set_value(value_b)
        self.assertEquals(cursor_p.update(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + self.timestamp_str(130))

        # Commit some more updates.
        self.session.begin_transaction()
        cursor.set_key(ds.key(nrows + 4))
        cursor.set_value(value_b)
        self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(140))
        self.session.begin_transaction()
        cursor.set_key(ds.key(nrows + 5))
        cursor.set_value(value_b)
        self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(150))

        # Move the oldest and the stable timestamp to the latest.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(155))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(155))

        # Commit an update newer than the stable timestamp.
        self.session.begin_transaction()
        cursor.set_key(ds.key(nrows + 6))
        cursor.set_value(value_b)
        self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(160))

        # Take a checkpoint here, so that prepared transaction will not be durable..
        self.session.checkpoint()

        # Commit the prepared transaction.
        session_p.commit_transaction('commit_timestamp=' + self.timestamp_str(140) + ',durable_timestamp=' + self.timestamp_str(160))

        # Take a backup and check the values.
        self.backup_dir = os.path.join(self.home, "WT_BACKUP")
        self.backup(self.backup_dir)
        backup_conn = self.wiredtiger_open(self.backup_dir)
        session_b = backup_conn.open_session()
        cursor_b = session_b.open_cursor(uri)

        # Committed non - prepared transactions data should be seen.
        cursor_b.set_key(ds.key(nrows + 1))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_b)
        cursor_b.set_key(ds.key(nrows + 2))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_b)
        # Committed prepared transaction data should not be seen.
        cursor_b.set_key(ds.key(nrows + 3))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_a)
        # Committed non - prepared transactions data should be seen.
        cursor_b.set_key(ds.key(nrows + 4))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_b)
        cursor_b.set_key(ds.key(nrows + 5))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_b)

        # Committed transactions newer to the stable timestamp should not be seen.
        cursor_b.set_key(ds.key(nrows + 6))
        self.assertEquals(cursor_b.search(), 0)
        self.assertEquals(cursor_b.get_value(),value_a)

        # close sessions.
        cursor_p.close()
        session_p.close()
        cursor.close()
        self.session.close()
        cursor_b.close()
        session_b.close()

    def test_older_prepare_updates(self):
        # Create a small table.
        uri = "table:test"
        nrows = 100
        ds = SimpleDataSet(
            self, uri, nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = b"aaaaa" * 100
            value_b = b"bbbbb" * 100

        # Initially load huge data
        cursor = self.session.open_cursor(uri)
        for i in range(1, 10000):
            cursor.set_key(ds.key(nrows + i))
            cursor.set_value(value_a)
            self.assertEquals(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # Check if txn_visible_all is working properly, when an active oldest
        # transaction is a prepared transaction and the oldest timestamp
        # advances beyond the prepared timestamp.
        self.older_prepare_updates(uri, ds, nrows, value_a, value_b)

if __name__ == '__main__':
    wttest.run()
