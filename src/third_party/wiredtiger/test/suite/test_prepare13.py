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
# [TEST_TAGS]
# truncate:prepare
# [END_TAGS]
#
# test_prepare13.py
#   Fast-truncate fails when a page contains prepared updates.
import wiredtiger, wttest
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios

class test_prepare13(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=10MB'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_prepare(self):
        nrows = 20000

        if self.value_format == '8t':
            replacement_value = 199
        else:
            replacement_value = "replacement_value"

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Create a large table with lots of pages.
        uri = "table:test_prepare13"
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, 'allocation_size=512,leaf_page_max=512,' + config)
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)
        cursor.close()

        # Prepare a record.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        cursor[simple_key(cursor, 1000)] = replacement_value
        cursor.close()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(10))

        try:
            # Pin oldest and stable to timestamp 10.
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
                ',stable_timestamp=' + self.timestamp_str(10))

            # Open a separate session and cursor and perform updates to let prepared update to evict.
            s = self.conn.open_session()
            cursor = s.open_cursor(uri, None)
            for i in range(2000, nrows):
                s.begin_transaction()
                cursor[simple_key(cursor, i)] = simple_value(cursor, i)
                s.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
            cursor.close()

            # Truncate the middle chunk and expect a conflict.
            msg = preparemsg = '/conflict between concurrent operations/'
            s.begin_transaction()
            c1 = s.open_cursor(uri, None)
            c1.set_key(simple_key(c1, 100))
            c2 = s.open_cursor(uri, None)
            c2.set_key(simple_key(c1, nrows))
            self.assertRaisesException(
                wiredtiger.WiredTigerError, lambda:s.truncate(None, c1, c2, None), msg)
            c1.close()
            c2.close()
            s.rollback_transaction()

        finally:
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(50))
            self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(50))
            self.session.commit_transaction()

        s.close()

if __name__ == '__main__':
    wttest.run()
