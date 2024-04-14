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

# test_truncate25.py
# Ensure that any history store records are correctly removed when doing
# a truncate operation with no timestamp.

class test_truncate25(wttest.WiredTigerTestCase):
    uri = 'table:test_truncate25'
    conn_config = 'statistics=(all)'
    nrows = 10000

    def test_truncate25(self):
        ds = SimpleDataSet(self, self.uri, 0, key_format='i', value_format='S')
        ds.populate()

        # Insert a large amount of data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows):
            cursor[ds.key(i)] = str(30)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(30)}')

        # Insert some more data at a later timestamp.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[ds.key(i)] = str(50)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(50)}')

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(50)}')

        self.session.checkpoint()
        self.reopen_conn()

        # Do a truncate operation with no timestamp on some portion of the key range.
        self.session.begin_transaction('no_timestamp=true')

        c1 = ds.open_cursor(self.uri, None)
        c1.set_key(ds.key(5000))
        c2 = ds.open_cursor(self.uri, None)
        c2.set_key(ds.key(8000))
        self.session.truncate(None, c1, c2, None)

        self.session.commit_transaction()

        # Ensure that fast-truncate did not happen since the data is not globally visible.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[wiredtiger.stat.conn.rec_page_delete_fast][2]
        self.assertEqual(fastdelete_pages, 0)

        # Re-insert some data at a later timestamp.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows):
            cursor[ds.key(i)] = str(60)
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(60)}')
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(60)}')
        self.session.checkpoint()

        self.reopen_conn()

        # Attempt to read the value of the key at timestamp 30. It should no longer
        # exist in the history store.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        cursor = self.session.open_cursor(self.uri)
        for i in range(5000, 8000):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
