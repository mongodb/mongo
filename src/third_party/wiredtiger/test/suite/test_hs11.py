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
from wtscenario import make_scenarios
from wiredtiger import stat

# test_hs11.py
# Ensure that updates without timestamps clear the history store records.
class test_hs11(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='S')),
        ('string-row', dict(key_format='S', value_format='S')),
    ]
    update_type_values = [
        ('deletion', dict(update_type='deletion')),
        ('update', dict(update_type='update'))
    ]
    long_running_txn_values = [
       ('long-running', dict(long_run_txn=True)),
        ('no-long-running', dict(long_run_txn=False))
    ]
    last_update_type_values = [
        ('modify', dict(modify=True)),
        ('no-modify', dict(modify=False))
    ]
    nrows = [
        ('small-nrows', dict(nrows=100)),
        ('large-nrows', dict(nrows=10000))
    ]
    scenarios = make_scenarios(format_values, update_type_values,long_running_txn_values, last_update_type_values, nrows)
    timestamps = 5

    def create_key(self, i):
        if self.key_format == 'S':
            return str(i)
        return i

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def evict_cursor(self, uri, nrows):
        s = self.conn.open_session()
        s.begin_transaction()
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(self.create_key(i))
            evict_cursor.search()
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()

    def test_non_ts_updates_clears_hs(self):
        uri = 'table:test_hs11'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500
            mod_value = 'm' + 'a' * 499

        # Apply a series of updates from timestamps 1-4.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        for ts in range(1, self.timestamps):
            for i in range(1, self.nrows):
                with self.transaction(commit_timestamp = ts):
                    cursor[self.create_key(i)] = value1

        # Reconcile and flush versions 1-3 to the history store.
        self.session.checkpoint()
        self.evict_cursor(uri, self.nrows)

        # Apply a modify update at timestamp 5.
        if self.modify and self.value_format != '8t':
            for i in range(1, self.nrows):
                with self.transaction(commit_timestamp = 5):
                    cursor.set_key(self.create_key(i))
                    cursor.modify([wiredtiger.Modify("m", 0, 1)])
            self.timestamps += 1

        # Start a long running transaction at timestamp 5. 
        if self.long_run_txn:
            session2 = self.conn.open_session()
            session2.begin_transaction('read_timestamp=5')

        # Apply an update without timestamp. If we have a long running transaction this update 
        # should not be globally visible until that transaction has ended.
        for i in range(1, self.nrows):
            self.session.begin_transaction('no_timestamp=true')
            if i % 2 == 0:
                if self.update_type == 'deletion':
                    cursor.set_key(self.create_key(i))
                    cursor.remove()
                else:
                    cursor[self.create_key(i)] = value2
            self.session.commit_transaction()

        # Reconcile and remove the obsolete entries.
        self.session.checkpoint()
        if self.long_run_txn:
            session2.rollback_transaction()
            
        # At this point any updates with no timestamp should be globally visible.
        self.evict_cursor(uri, self.nrows)

        # Now apply an update at timestamp 10.
        for i in range(1, self.nrows):
            with self.transaction(commit_timestamp = 10):
                cursor[self.create_key(i)] = value2

        self.session.checkpoint()

        # Ensure that we blew away history store content.
        for ts in range(1, self.timestamps):
            with self.transaction(read_timestamp = ts, rollback = True):
                for i in range(1, self.nrows):
                    if i % 2 == 0:
                        if self.update_type == 'deletion':
                            cursor.set_key(self.create_key(i))
                            if self.value_format == '8t':
                                self.assertEqual(cursor.search(), 0)
                                self.assertEqual(cursor.get_value(), 0)
                            else:
                                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                        else:
                            self.assertEqual(cursor[self.create_key(i)], value2)
                    else:
                        if ts == 5 and self.modify and self.value_format != '8t':
                            self.assertEqual(cursor[self.create_key(i)], mod_value)
                        else:
                            self.assertEqual(cursor[self.create_key(i)], value1)

        if self.update_type == 'deletion':
            hs_truncate = self.get_stat(stat.conn.cache_hs_key_truncate_onpage_removal)
            self.assertGreater(hs_truncate, 0)

    def test_ts_updates_donot_clears_hs(self):
        uri = 'table:test_hs11'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500
            mod_value = 'm' + 'a' * 499

        # Apply a series of updates from timestamps 1-4.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        for ts in range(1, self.timestamps):
            for i in range(1, self.nrows):
                with self.transaction(commit_timestamp = ts):
                    cursor[self.create_key(i)] = value1

        # Reconcile and flush versions 1-3 to the history store.
        self.session.checkpoint()

        # Apply a modify update at timestamp 5.
        if self.modify and self.value_format != '8t':
            for i in range(1, self.nrows):
                with self.transaction(commit_timestamp = 5):
                    cursor.set_key(self.create_key(i))
                    cursor.modify([wiredtiger.Modify("m", 0, 1)])
            self.timestamps += 1

        # Remove the key with timestamp 10.
        for i in range(1, self.nrows):
            if i % 2 == 0:
                with self.transaction(commit_timestamp = 10):
                    cursor.set_key(self.create_key(i))
                    cursor.remove()

        # Reconcile and remove the obsolete entries.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Now apply an update at timestamp 20.
        for i in range(1, self.nrows):
                with self.transaction(commit_timestamp = 20):
                    cursor[self.create_key(i)] = value2

        # Ensure that we didn't select old history store content even if it is not blew away.
        with self.transaction(read_timestamp = 10, rollback = True):
            for i in range(1, self.nrows):
                if i % 2 == 0:
                    cursor.set_key(self.create_key(i))
                    if self.value_format == '8t':
                        self.assertEqual(cursor.search(), 0)
                        self.assertEqual(cursor.get_value(), 0)
                    else:
                        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                else:
                    if self.modify and self.value_format != '8t':
                        self.assertEqual(cursor[self.create_key(i)], mod_value)
                    else:
                        self.assertEqual(cursor[self.create_key(i)], value1)

        hs_truncate = self.get_stat(stat.conn.cache_hs_key_truncate_onpage_removal)
        self.assertEqual(hs_truncate, 0)
