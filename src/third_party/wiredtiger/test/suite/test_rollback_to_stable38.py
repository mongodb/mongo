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

import wttest
from helper import simulate_crash_restart
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable38.py
#
# Test using fast truncate to delete the whole tree of records from the history store

class test_rollback_to_stable38(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),cache_size=50MB'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig='')),
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
    ]
    scenarios = make_scenarios(format_values)

    def check(self, ds, value, nrows, ts):
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        self.session.rollback_transaction()
        cursor.close()

    def test_rollback_to_stable38(self):
        nrows = 1000000

        # Create a table.
        uri = "table:rollback_to_stable38"
    
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin a transaction
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Write a value to table.
        cursor1 = self.session.open_cursor(ds.uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor1[ds.key(i)] = value_a
            self.session.commit_transaction()

         # Write another value to table.
        cursor1 = self.session.open_cursor(ds.uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor1[ds.key(i)] = value_a
            self.session.commit_transaction()

        # Do a checkpoint
        self.session.checkpoint()

        session2.rollback_transaction()
        session2.close()

        # Roll back via crashing.
        simulate_crash_restart(self, ".", "RESTART")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_btree_truncate = stat_cursor[stat.conn.cache_hs_btree_truncate][2]
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        self.assertGreater(hs_btree_truncate, 0)
        self.assertGreater(fastdelete_pages, 0)
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
