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

# test_prepare10.py
# Test to ensure prepared tombstones are properly aborted even when they are written
# to the data store.
class test_prepare10(wttest.WiredTigerTestCase):
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
            self.assertEquals(cursor.insert(), 0)
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

    def check(self, ds, uri, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('ignore_prepare=true,read_timestamp=' + self.timestamp_str(ts))
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            self.assertEquals(cursor.search(), 0)
            self.assertEquals(cursor.get_value(),value)
        self.session.commit_transaction()
        cursor.close()

    def check_not_found(self, ds, uri, nrows, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('ignore_prepare=true,read_timestamp=' + self.timestamp_str(ts))
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            if self.value_format == '8t':
                # In FLCS, deleted values read back as 0.
                self.assertEquals(cursor.search(), 0)
                self.assertEquals(cursor.get_value(), 0)
            else:
                self.assertEquals(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()
        cursor.close()

    def test_prepare_rollback_retrieve_time_window(self):
        # Create a small table.
        uri = "table:test_prepare10"
        nrows = 1000
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
           value_a = 97
           value_b = 98
           value_c = 99
        else:
            value_a = b"aaaaa" * 100
            value_b = b"bbbbb" * 100
            value_c = b"ccccc" * 100

        # Commit some updates along with a prepared update, which is not resolved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Initially load huge data
        self.updates(ds, uri, nrows, value_a, 20)
        # Add some more updates
        self.updates(ds, uri, nrows, value_b, 30)

        # Checkpoint
        self.session.checkpoint()

        # Validate that we do see the correct value.
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        for i in range(1, nrows):
            cursor2.set_key(ds.key(i))
            self.assertEquals(cursor2.search(), 0)
            self.assertEquals(cursor2.get_value(), value_b)
        session2.commit_transaction()

        # Reset the cursor.
        cursor2.reset()
        session2.begin_transaction()

        # Remove all keys
        self.removes(ds, uri, nrows, 40)

        # Validate that we do see the correct value.
        session3 = self.setUpSessionOpen(self.conn)
        cursor3 = session3.open_cursor(uri)
        session3.begin_transaction()
        for i in range(1, nrows):
            cursor3.set_key(ds.key(i))
            if self.value_format == '8t':
                # In FLCS deleted records read back as 0.
                self.assertEquals(cursor3.search(), 0)
                self.assertEquals(cursor3.get_value(), 0)
            else:
                self.assertEquals(cursor3.search(), wiredtiger.WT_NOTFOUND)
        session3.commit_transaction()

        # Reset the cursor.
        cursor3.reset()
        session3.begin_transaction()

        # Insert the updates from a prepare session and keep it open.
        session_p = self.conn.open_session()
        cursor_p = session_p.open_cursor(uri)
        session_p.begin_transaction()
        for i in range(1, nrows):
            cursor_p.set_key(ds.key(i))
            cursor_p.set_value(value_c)
            self.assertEquals(cursor_p.insert(), 0)
        session_p.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50))

        self.check(ds, uri, nrows, value_a, 20)
        self.check(ds, uri, nrows, value_b, 35)
        self.check_not_found(ds, uri, nrows, 60)

        #rollback the prepared session
        session_p.rollback_transaction()

        self.check(ds, uri, nrows, value_a, 20)
        self.check(ds, uri, nrows, value_b, 35)
        self.check_not_found(ds, uri, nrows, 60)

        # session2 still can see the value_b
        for i in range(1, nrows):
            cursor2.set_key(ds.key(i))
            self.assertEquals(cursor2.search(), 0)
            self.assertEquals(cursor2.get_value(), value_b)
        session2.commit_transaction()

        # session3 still can't see a value
        for i in range(1, nrows):
            cursor3.set_key(ds.key(i))
            if self.value_format == '8t':
                # In FLCS, deleted records read back as 0.
                self.assertEquals(cursor3.search(), 0)
                self.assertEquals(cursor3.get_value(), 0)
            else:
                self.assertEquals(cursor3.search(), wiredtiger.WT_NOTFOUND)
        session3.commit_transaction()

        # close sessions.
        cursor_p.close()
        session_p.close()
        cursor2.close()
        session2.close()
        cursor3.close()
        session3.close()
        self.session.close()

if __name__ == '__main__':
    wttest.run()
