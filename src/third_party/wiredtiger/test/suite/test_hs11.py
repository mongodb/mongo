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

import wiredtiger, wttest
from wtscenario import make_scenarios
from wiredtiger import stat

def timestamp_str(t):
    return '%x' % t

# test_hs11.py
# Ensure that updates without timestamps clear the history store records.
class test_hs11(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'
    session_config = 'isolation=snapshot'
    scenarios = make_scenarios([
        ('deletion', dict(update_type='deletion')),
        ('update', dict(update_type='update')),
    ])

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_non_ts_updates_clears_hs(self):
        uri = 'table:test_hs11'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)

        value1 = 'a' * 500
        value2 = 'b' * 500

        # Apply a series of updates from timestamps 1-4.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        for ts in range(1, 5):
            for i in range(1, 10000):
                self.session.begin_transaction()
                cursor[str(i)] = value1
                self.session.commit_transaction('commit_timestamp=' + timestamp_str(ts))

        # Reconcile and flush versions 1-3 to the history store.
        self.session.checkpoint()

        # Apply an update without timestamp.
        for i in range(1, 10000):
            if i % 2 == 0:
                if self.update_type == 'deletion':
                    cursor.set_key(str(i))
                    cursor.remove()
                else:
                    cursor[str(i)] = value2

        # Reconcile and remove the obsolete entries.
        self.session.checkpoint()

        # Now apply an update at timestamp 10.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(10))

        # Ensure that we blew away history store content.
        for ts in range(1, 5):
            self.session.begin_transaction('read_timestamp=' + timestamp_str(ts))
            for i in range(1, 10000):
                if i % 2 == 0:
                    if self.update_type == 'deletion':
                        cursor.set_key(str(i))
                        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                    else:
                        self.assertEqual(cursor[str(i)], value2)
                else:
                    self.assertEqual(cursor[str(i)], value1)
            self.session.rollback_transaction()

        if self.update_type == 'deletion':
            hs_truncate = self.get_stat(stat.conn.cache_hs_key_truncate_onpage_removal)
            self.assertGreater(hs_truncate, 0)

    def test_ts_updates_donot_clears_hs(self):
        uri = 'table:test_hs11'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)

        value1 = 'a' * 500
        value2 = 'b' * 500

        # Apply a series of updates from timestamps 1-4.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        for ts in range(1, 5):
            for i in range(1, 10000):
                self.session.begin_transaction()
                cursor[str(i)] = value1
                self.session.commit_transaction('commit_timestamp=' + timestamp_str(ts))

        # Reconcile and flush versions 1-3 to the history store.
        self.session.checkpoint()

        # Remove the key with timestamp 10.
        for i in range(1, 10000):
            if i % 2 == 0:
                self.session.begin_transaction()
                cursor.set_key(str(i))
                cursor.remove()
                self.session.commit_transaction('commit_timestamp=' + timestamp_str(10))

        # Reconcile and remove the obsolete entries.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10))
        self.session.checkpoint()

        # Now apply an update at timestamp 20.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(20))

        # Ensure that we didn't select old history store content even if it is not blew away.
        self.session.begin_transaction('read_timestamp=' + timestamp_str(10))
        for i in range(1, 10000):
            if i % 2 == 0:
                cursor.set_key(str(i))
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(cursor[str(i)], value1)
        self.session.rollback_transaction()

        hs_truncate = self.get_stat(stat.conn.cache_hs_key_truncate_onpage_removal)
        self.assertEqual(hs_truncate, 0)
