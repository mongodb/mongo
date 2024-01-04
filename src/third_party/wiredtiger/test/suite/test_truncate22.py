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
from wtdataset import SimpleDataSet

# test_truncate22.py
# Test that we can set the commit timestamp before performing fast truncate.
class test_truncate22(wttest.WiredTigerTestCase):
    uri = 'table:test_truncate22'
    conn_config = 'statistics=(all)'
    key_format_values = (
        ('column', dict(key_format='r', value_format='S')),
        ('integer-row', dict(key_format='i', value_format='S'))
    )
    scenarios = make_scenarios(key_format_values)
    nrows = 10000

    def test_truncate22(self):
        # Create a table.
        ds = SimpleDataSet(self, self.uri, 0,
                        key_format=self.key_format,
                        value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(1)}')

        # Insert a large amount of data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows):
            cursor[ds.key(i)] = str(i)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')

        # Reopen the connection so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        self.session.begin_transaction()
        self.session.timestamp_transaction(f'commit_timestamp={self.timestamp_str(5)}')

        c1 = ds.open_cursor(self.uri, None)
        c1.set_key(ds.key(1))
        c2 = ds.open_cursor(self.uri, None)
        c2.set_key(ds.key(self.nrows // 2))
        self.session.truncate(None, c1, c2, None)

        self.session.commit_transaction()

        # Advance stable.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        # Check that we removed the first half of the dataset as part of the truncation.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows // 2):
            cursor.set_key(ds.key(i))
            self.assertNotEqual(cursor.search(), 0)
