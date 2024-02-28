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

import threading, time
import wttest
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint32.py
#
# Test that skipping in-memory reconciled deleted pages as part of the tree walk.
class test_checkpoint32(wttest.WiredTigerTestCase):

    format_values = [
        # FLCS doesn't support skipping pages based on aggregated time.
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]

    scenarios = make_scenarios(format_values)

    def check(self, ds, nrows, value):
        cursor = self.session.open_cursor(ds.uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint32'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction()

        # Create a reader transaction that will not be able to see what happens next.
        # We don't need to do anything with this; it just needs to exist.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Now remove all data.
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            self.session.commit_transaction()

        # Checkpoint.
        self.session.checkpoint()

        # Get the existing in-memory delete page skip statistic value.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prev_cur_inmem_del_page_skip = stat_cursor[stat.conn.cursor_tree_walk_inmem_del_page_skip][2]
        stat_cursor.close()

        # Now read the removed data.
        self.check(ds, 0, value_a)

        # Get the new in-memory delete page skip statistic value.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cur_inmem_del_page_skip = stat_cursor[stat.conn.cursor_tree_walk_inmem_del_page_skip][2]
        stat_cursor.close()

        self.assertGreater(cur_inmem_del_page_skip, prev_cur_inmem_del_page_skip)

        # Tidy up.
        session2.rollback_transaction()
        session2.close()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
