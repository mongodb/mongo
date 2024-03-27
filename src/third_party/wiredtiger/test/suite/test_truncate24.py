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
# test_truncate24.py
#   Test commit timestamp is not overwritten when the reinstantiated
#   deletes are committed.

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_truncate24(wttest.WiredTigerTestCase):
    key_format_values = [
        ('row', dict(key_format='i', value_format='i')),
        ('var', dict(key_format='r', value_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)
    def test_truncate24(self):
        uri = 'table:truncate24'
        ds = SimpleDataSet(self, uri, 1000000, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Reopen the connection to flush the cache so we can fast-truncate.
        self.reopen_conn()

        cursor = self.session.open_cursor(uri)

        # Truncate the data at timestamp 10 but commit at 20.
        self.session.begin_transaction()
        self.session.timestamp_transaction("commit_timestamp=" + self.timestamp_str(10))
        self.session.truncate(uri, None, None, None)
        # Reload the deleted pages to memory.
        for i in (1, 1000000):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Check stats to make sure we fast-deleted at least one page.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[wiredtiger.stat.conn.rec_page_delete_fast][2]
        self.assertGreater(fastdelete_pages, 0)

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10))
        # Verify we don't see the data at timestamp 10.
        for i in (1, 1000000):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
