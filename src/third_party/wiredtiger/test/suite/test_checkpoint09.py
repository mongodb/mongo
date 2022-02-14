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
#
# test_checkpoint09.py
# Test that reconcile clears the time window of on-disk cell if it is obsolete.

import wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_checkpoint09(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='u')),
        ('row_string', dict(key_format='S', value_format='u')),
    ]

    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, uri, value, ds, nrows, skip_count, commit_ts):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            if (i % skip_count) == 0:
                session.begin_transaction()
                cursor[ds.key(i)] = value
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)
        cursor.close()

    def evict_cursor(self, uri, ds, nrows):
        s = self.conn.open_session()
        s.begin_transaction()
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(ds.key(i))
            self.assertEquals(evict_cursor.search(), 0)
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()

    def test_checkpoint09(self):
        uri = 'table:ckpt09'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(self, uri, nrows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable timestamp to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        if self.value_format == '8t':
            value1 = 86
            value2 = 87
            value3 = 88
        else:
            value1 = b"aaaaa" * 100
            value2 = b"bbbbb" * 100
            value3 = b"ccccc" * 100

        self.large_updates(uri, value1, ds, nrows, 1, 10)
        self.check(value1, uri, nrows)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        val = self.get_stat(stat.conn.rec_time_window_start_ts)
        self.assertEqual(val, nrows)

        self.evict_cursor(uri, ds, nrows)
        self.large_updates(uri, value2, ds, nrows, 10, 20)

        # Pin oldest timestamp to 10 and stable timestamp to 20.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint(None)

        val = self.get_stat(stat.conn.rec_time_window_start_ts)
        self.assertEqual(val, nrows + nrows/10)

        self.evict_cursor(uri, ds, nrows)
        self.large_updates(uri, value3, ds, nrows, 100, 30)

        # Pin oldest timestamp to 20 and stable timestamp to 30.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20) +
            ',stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()

        val = self.get_stat(stat.conn.rec_time_window_start_ts)
        self.assertEqual(val, nrows + nrows/10 + nrows/100)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
