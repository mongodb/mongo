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

# test_hs31.py
# Ensure that tombstone with out of order timestamp clear the history store records.
class test_hs31(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=5MB,statistics=(all)'
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        # ('column-fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='S')),
        ('string-row', dict(key_format='S', value_format='S')),
    ]

    ooo_values = [
        ('out-of-order', dict(ooo_value=True)),
        ('mixed-mode', dict(ooo_value=False)),
    ]

    globally_visible_before_ckpt_values = [
        ('globally_visible_before_ckpt', dict(globally_visible_before_ckpt=True)),
        ('no_globally_visible_before_ckpt', dict(globally_visible_before_ckpt=False)),
    ]

    scenarios = make_scenarios(format_values, ooo_values, globally_visible_before_ckpt_values)
    nrows = 1000

    def create_key(self, i):
        if self.key_format == 'S':
            return str(i)
        return i

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_ooo_tombstone_clear_hs(self):
        uri = 'file:test_hs31'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Apply a series of updates from timestamps 10-14.
        cursor = self.session.open_cursor(uri)
        for ts in range(10, 15):
            for i in range(1, self.nrows):
                self.session.begin_transaction()
                cursor[self.create_key(i)] = value1
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

        # Reconcile and flush versions 10-13 to the history store.
        self.session.checkpoint()

        # Evict the data from the cache.
        self.session.begin_transaction()
        cursor2 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")
        for i in range(1, self.nrows):
            cursor2.set_key(self.create_key(i))
            cursor2.search()
            cursor2.reset()
        self.session.rollback_transaction()

        if not self.ooo_value:
            self.session.breakpoint()
            # Start a long running transaction to stop the oldest id being advanced.
            session2 = self.conn.open_session()
            session2.begin_transaction()
            long_cursor = session2.open_cursor(uri, None)
            long_cursor[self.create_key(self.nrows + 10)] = value1
            long_cursor.reset()
            long_cursor.close()

        # Remove the key with an ooo or mm timestamp.
        for i in range(1, self.nrows):
            self.session.begin_transaction()
            cursor.set_key(self.create_key(i))
            cursor.remove()
            if self.ooo_value:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
            else:
                self.session.commit_transaction()

        if not self.globally_visible_before_ckpt:
            # Reconcile to write the stop time window.
            self.session.checkpoint()

        if not self.ooo_value:
            self.session.breakpoint()
            # Ensure that old reader can read the history content.
            long_cursor = session2.open_cursor(uri, None)
            for i in range(1, self.nrows):
                long_cursor.set_key(self.create_key(i))
                self.assertEqual(long_cursor.search(), 0)
                self.assertEqual(long_cursor.get_value(), value1)
            long_cursor.reset()
            long_cursor.close()

            # Rollback the long running transaction.
            session2.rollback_transaction()
            session2.close()

        # Pin oldest and stable to timestamp 5 so that the ooo tombstone is globally visible.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Reconcile and remove the obsolete entries.
        self.session.checkpoint()

        # Evict the data from the cache.
        self.session.begin_transaction()
        cursor2 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")
        for i in range(1, self.nrows):
            cursor2.set_key(self.create_key(i))
            if self.value_format == '8t':
                self.assertEqual(cursor2.search(), 0)
            else:
                self.assertEqual(cursor2.search(), wiredtiger.WT_NOTFOUND)
            cursor2.reset()
        self.session.rollback_transaction()

        # Now apply an insert at timestamp 20.
        for i in range(1, self.nrows):
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Ensure that we blew away history store content.
        for ts in range(10, 15):
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
            for i in range(1, self.nrows):
                cursor.set_key(self.create_key(i))
                if self.value_format == '8t':
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), 0)
                else:
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            self.session.rollback_transaction()

        hs_truncate = self.get_stat(stat.conn.cache_hs_key_truncate_onpage_removal)
        self.assertGreater(hs_truncate, 0)
