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

import time
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs07.py
# Test that the history store sweep cleans the obsolete history store entries and gives expected results.
class test_hs07(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = ('cache_size=50MB,eviction_updates_trigger=95,'
                   'eviction_updates_target=80,log=(enabled)')

    format_values = (
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='S'))
    )
    scenarios = make_scenarios(format_values)

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records, we'll hang if the history store table isn't working.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.rollback_transaction()
        self.assertEqual(count, nrows)

    def test_hs(self):
        nrows = 10000

        # Create a table without logging to ensure we get "skew_newest" history store eviction
        # behavior.
        uri = "table:las07_main"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()

        uri2 = "table:las07_extra"
        ds2 = SimpleDataSet(
            self, uri2, 0, key_format=self.key_format, value_format=self.value_format)
        ds2.populate()

        if self.value_format == '8t':
            bigvalue = 97
            bigvalue2 = 100
        else:
            bigvalue = "aaaaa" * 100
            bigvalue2 = "ddddd" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri, bigvalue, ds, nrows, 1)

        # Check that all updates are seen
        self.check(bigvalue, uri, nrows, 100)

        # Force out most of the pages by updating a different tree
        self.large_updates(uri2, bigvalue, ds2, nrows, 100)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue, uri, nrows, 100)

        # Pin oldest and stable to timestamp 100.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(100) +
            ',stable_timestamp=' + self.timestamp_str(100))

        # Sleep here to let that sweep server to trigger cleanup of obsolete entries.
        time.sleep(10)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue, uri, nrows, 100)

        # Load a slight modification with a later timestamp.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(105)
                cursor.update()
            else:
                mods = [wiredtiger.Modify('A', 10, 1)]
                self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(110))

        # Load a slight modification with a later timestamp.
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(106)
                cursor.update()
            else:
               mods = [wiredtiger.Modify('B', 20, 1)]
               self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(120))

        # Load a slight modification with a later timestamp.
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(107)
                cursor.update()
            else:
                mods = [wiredtiger.Modify('C', 30, 1)]
                self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(130))
        cursor.close()

        # Second set of update operations with increased timestamp
        self.large_updates(uri, bigvalue2, ds, nrows, 200)

        # Force out most of the pages by updating a different tree
        self.large_updates(uri2, bigvalue2, ds2, nrows, 200)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue2, uri, nrows, 200)

        # Pin oldest and stable to timestamp 300.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(200) +
            ',stable_timestamp=' + self.timestamp_str(200))

        # Sleep here to let that sweep server to trigger cleanup of obsolete entries.
        time.sleep(10)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue2, uri, nrows, 200)

        # Load a slight modification with a later timestamp.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(105)
                cursor.update()
            else:
                mods = [wiredtiger.Modify('A', 10, 1)]
                self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(210))

        # Load a slight modification with a later timestamp.
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(106)
                cursor.update()
            else:
                mods = [wiredtiger.Modify('B', 20, 1)]
                self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(220))

        # Load a slight modification with a later timestamp.
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor.set_key(i)
            if self.value_format == '8t':
                cursor.set_value(107)
                cursor.update()
            else:
                mods = [wiredtiger.Modify('C', 30, 1)]
                self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(230))
        cursor.close()

        # Third set of update operations with increased timestamp
        self.large_updates(uri, bigvalue, ds, nrows, 300)

        # Force out most of the pages by updating a different tree
        self.large_updates(uri2, bigvalue, ds2, nrows, 300)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue, uri, nrows, 300)

        # Pin oldest and stable to timestamp 400.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(300) +
            ',stable_timestamp=' + self.timestamp_str(300))

        # Sleep here to let that sweep server to trigger cleanup of obsolete entries.
        time.sleep(10)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue, uri, nrows, 300)

if __name__ == '__main__':
    wttest.run()
