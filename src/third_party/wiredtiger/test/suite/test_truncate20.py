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
import os, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate20.py
#
# Test to mimic oplog workload in MongoDB. Ensure the deleted pages are
# cleaned up on disk and we are not using excessive disk space.
class test_truncate20(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),log=(enabled=true)'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
        ('row_string', dict(key_format='S', value_format='S')),
    ]
    scenarios = make_scenarios(format_values)

    def append_rows(self, uri, ds, start_row, nrows, value):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(start_row, start_row + nrows + 1):
            cursor[ds.key(i)] = value
            if i % 2 == 0:
                self.session.commit_transaction()
                self.session.begin_transaction()
        self.session.commit_transaction()
        cursor.close()

    def do_truncate(self, ds, start_row, nrows):
        self.session.begin_transaction()
        hicursor = self.session.open_cursor(ds.uri)
        hicursor.set_key(ds.key(start_row + nrows))
        self.session.truncate(None, None, hicursor, None)
        self.session.commit_transaction()

    def evict_cursor(self, uri, ds, start_num, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (start_num, start_num + nrows + 1):
            evict_cursor.set_key(ds.key(i))
            evict_cursor.search()
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    @wttest.longtest('large number of rows to truncate and checkpoint')
    def test_truncate(self):
        uri = 'table:oplog'
        nrows = 1000000

        # Create a table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        value_a = "aaaaa" * 100

        # Write some data
        self.append_rows(uri, ds, 1, nrows, value_a)
        self.evict_cursor(uri, ds, 1, nrows)

        trunc_rows = 0
        start_num = 1
        end_num = nrows
        for i in range(1, 50):
            # Session for checkpoint
            session2 = self.conn.open_session()
            # Session for long running transaction, to make truncate not globally visible
            session3 = self.conn.open_session()
            # Start a long running transaction
            session3.begin_transaction()
            trunc_rows = 10000

            self.do_truncate(ds, start_num, trunc_rows)

            # Check stats to make sure we fast-deleted at least one page.
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]

            self.assertGreater(fastdelete_pages, 0)

            self.append_rows(uri, ds, end_num, trunc_rows, value_a)

            end_num = end_num + trunc_rows
            start_num = start_num + trunc_rows

            self.evict_cursor(uri, ds, start_num, nrows)

            # Take a checkpoint.
            session2.checkpoint()

            # Ensure the datasize is smaller than 600M
            self.assertGreater(600000000, os.path.getsize("oplog.wt"))
            session3.rollback_transaction()

        # Ensure the datasize is smaller than 600M
        self.assertGreater(600000000, os.path.getsize("oplog.wt"))

if __name__ == '__main__':
    wttest.run()
