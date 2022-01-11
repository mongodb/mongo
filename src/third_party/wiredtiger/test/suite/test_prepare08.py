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

# test_prepare08.py
# Test to ensure prepared tombstones are properly aborted/committed even when they are written
# to the data store.
class test_prepare08(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=10MB,eviction_dirty_trigger=80,eviction_updates_trigger=80'

    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='u')),
    ]

    scenarios = make_scenarios(format_values)

    def updates(self, ds, uri, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            cursor.set_value(value)
            self.assertEquals(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def removes(self, ds, uri, nrows, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            self.assertEquals(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def check(self, ds, uri, nrows, value, ts, release_evict):
        if release_evict:
            cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        else:
            cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('ignore_prepare=true,read_timestamp=' + self.timestamp_str(ts))
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            if value == None:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEquals(cursor.search(), 0)
                self.assertEquals(cursor.get_value(),value)
            cursor.reset()
        self.session.commit_transaction()
        cursor.close()

    def test_prepare_delete_rollback(self):
        nrows = 2000

        # Create a small table.
        uri_1 = "table:test_prepare08_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format)
        ds_1.populate()

        uri_2 = "table:test_prepare08_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format)
        ds_2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
            value_e = 101
        else:
            value_a = b"aaaaa" * 100
            value_b = b"bbbbb" * 100
            value_c = b"ccccc" * 100
            value_d = b"ddddd" * 100
            value_e = b"eeeee" * 100

        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Initially load huge data
        self.updates(ds_1, uri_1, nrows, value_a, 20)
        self.updates(ds_2, uri_2, nrows, value_a, 20)

        # Add some more updates
        self.updates(ds_1, uri_1, nrows, value_b, 30)
        self.updates(ds_2, uri_2, nrows, value_b, 30)

        # Verify the updates
        self.check(ds_1, uri_1, nrows, value_a, 20, False)
        self.check(ds_1, uri_1, nrows, value_b, 30, False)

        self.check(ds_2, uri_2, nrows, value_a, 20, False)
        self.check(ds_2, uri_2, nrows, value_b, 30, False)

        # Checkpoint
        self.session.checkpoint()

        # Remove the updates from a prepare session and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri_1)
        session_p.begin_transaction()
        for i in range(1, nrows):
            cursor_p.set_key(ds_1.key(i))
            self.assertEquals(cursor_p.remove(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))

        # Adding more updates to other table should trigger eviction on uri_1
        self.updates(ds_2, uri_2, nrows, value_c, 40)
        self.updates(ds_2, uri_2, nrows, value_d, 50)
        self.updates(ds_2, uri_2, nrows, value_e, 60)

        self.check(ds_1, uri_1, nrows, value_a, 20, True)
        self.check(ds_1, uri_1, nrows, value_b, 50, True)

        #rollback the prepared session
        session_p.rollback_transaction()

        self.check(ds_1, uri_1, nrows, value_a, 20, False)
        self.check(ds_1, uri_1, nrows, value_b, 50, False)

        # close sessions.
        cursor_p.close()
        session_p.close()
        self.session.close()

    def test_prepare_update_delete_commit(self):
        nrows = 2000

        # Create a small table.
        uri_1 = "table:test_prepare10_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format)
        ds_1.populate()

        # Create another small table.
        uri_2 = "table:test_prepare10_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format)
        ds_2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
            value_e = 101
        else:
            value_a = b"aaaaa" * 100
            value_b = b"bbbbb" * 100
            value_c = b"ccccc" * 100
            value_d = b"ddddd" * 100
            value_e = b"eeeee" * 100

        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Initially load huge data
        self.updates(ds_1, uri_1, nrows, value_a, 20)
        self.updates(ds_2, uri_2, nrows, value_a, 20)

        # Add some more updates
        self.updates(ds_1, uri_1, nrows, value_b, 30)
        self.updates(ds_2, uri_2, nrows, value_b, 30)

        # Verify the updates
        self.check(ds_1, uri_1, nrows, value_a, 20, False)
        self.check(ds_1, uri_1, nrows, value_b, 30, False)

        self.check(ds_2, uri_2, nrows, value_a, 20, False)
        self.check(ds_2, uri_2, nrows, value_b, 30, False)

        # Checkpoint
        self.session.checkpoint()

        # Remove the updates from a prepare session and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri_1)
        session_p.begin_transaction()
        for i in range(1, nrows):
            cursor_p.set_key(ds_1.key(i))
            cursor_p.set_value(value_c)
            self.assertEquals(cursor_p.update(), 0)
            cursor_p.set_key(ds_1.key(i))
            self.assertEquals(cursor_p.remove(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))

        # Adding more updates to other table should trigger eviction on uri_1
        self.updates(ds_2, uri_2, nrows, value_c, 40)
        self.updates(ds_2, uri_2, nrows, value_d, 50)
        self.updates(ds_2, uri_2, nrows, value_e, 60)

        self.check(ds_1, uri_1, nrows, value_a, 20, True)
        self.check(ds_1, uri_1, nrows, value_b, 30, True)
        self.check(ds_1, uri_1, nrows, value_b, 50, True)

        # Commit the prepared session
        session_p.commit_transaction('commit_timestamp=' + self.timestamp_str(50) + ',durable_timestamp=' + self.timestamp_str(60))

        self.check(ds_1, uri_1, nrows, value_a, 20, False)
        self.check(ds_1, uri_1, nrows, value_b, 30, False)
        self.check(ds_1, uri_1, 0, None, 50, False)

        # close sessions.
        cursor_p.close()
        session_p.close()
        self.session.close()

    def test_prepare_update_delete_commit_with_no_base_update(self):
        nrows = 2000

        # Create a small table.
        uri_1 = "table:test_prepare10_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format)
        ds_1.populate()

        # Create another small table.
        uri_2 = "table:test_prepare10_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format)
        ds_2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
            value_e = 101
        else:
            value_a = b"aaaaa" * 100
            value_b = b"bbbbb" * 100
            value_c = b"ccccc" * 100
            value_d = b"ddddd" * 100
            value_e = b"eeeee" * 100

        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Initially load huge data
        self.updates(ds_1, uri_1, nrows, value_a, 20)
        self.updates(ds_2, uri_2, nrows, value_a, 20)

        # Remove updates from one table and add some more updates to another table
        self.removes(ds_1, uri_1, nrows, 30)
        self.updates(ds_2, uri_2, nrows, value_b, 30)

        # Checkpoint
        self.session.checkpoint()

        # Remove the updates from a prepare session and and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri_1)
        session_p.begin_transaction()
        for i in range(1, nrows):
            cursor_p.set_key(ds_1.key(i))
            cursor_p.set_value(value_c)
            self.assertEquals(cursor_p.update(), 0)
            cursor_p.set_key(ds_1.key(i))
            cursor_p.set_value(value_d)
            self.assertEquals(cursor_p.update(), 0)
            cursor_p.set_key(ds_1.key(i))
            self.assertEquals(cursor_p.remove(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))

        # Adding more updates to other table should trigger eviction on uri_1
        self.updates(ds_2, uri_2, nrows, value_c, 40)
        self.updates(ds_2, uri_2, nrows, value_d, 50)
        self.updates(ds_2, uri_2, nrows, value_e, 60)

        self.check(ds_1, uri_1, nrows, value_a, 20, True)
        self.check(ds_1, uri_1, 0, None, 30, True)
        self.check(ds_1, uri_1, 0, None, 50, True)

        # Commit the prepared session
        session_p.commit_transaction('commit_timestamp=' + self.timestamp_str(50) + ',durable_timestamp=' + self.timestamp_str(60))

        self.check(ds_1, uri_1, nrows, value_a, 20, False)
        self.check(ds_1, uri_1, 0, None, 30, False)
        self.check(ds_1, uri_1, 0, None, 50, False)

        # close sessions.
        cursor_p.close()
        session_p.close()
        self.session.close()

if __name__ == '__main__':
    wttest.run()
