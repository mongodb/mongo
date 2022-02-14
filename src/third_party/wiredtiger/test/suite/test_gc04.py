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

from test_gc01 import test_gc_base
from wiredtiger import stat
from wtdataset import SimpleDataSet

# test_gc04.py
# Test that checkpoint must not clean the pages that are not obsolete.
class test_gc04(test_gc_base):
    conn_config = 'cache_size=50MB,statistics=(all)'

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_gc(self):
        nrows = 10000

        # Create a table.
        uri = "table:gc04"
        ds = SimpleDataSet(self, uri, 0, key_format="i", value_format="S")
        ds.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        bigvalue = "aaaaa" * 100
        bigvalue2 = "ddddd" * 100
        self.large_updates(uri, bigvalue, ds, nrows, 10)
        self.large_updates(uri, bigvalue2, ds, nrows, 20)

        # Checkpoint to ensure that the history store is populated.
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.cc_pages_evict), 0)
        self.assertEqual(self.get_stat(stat.conn.cc_pages_removed), 0)
        self.assertGreater(self.get_stat(stat.conn.cc_pages_visited), 0)

        self.large_updates(uri, bigvalue, ds, nrows, 30)

        # Checkpoint to ensure that the history store is populated.
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.cc_pages_evict), 0)
        self.assertEqual(self.get_stat(stat.conn.cc_pages_removed), 0)
        self.assertGreater(self.get_stat(stat.conn.cc_pages_visited), 0)

        self.large_updates(uri, bigvalue2, ds, nrows, 40)

        # Checkpoint to ensure that the history store is populated.
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.cc_pages_evict), 0)
        self.assertEqual(self.get_stat(stat.conn.cc_pages_removed), 0)
        self.assertGreater(self.get_stat(stat.conn.cc_pages_visited), 0)

        self.large_updates(uri, bigvalue, ds, nrows, 50)
        self.large_updates(uri, bigvalue2, ds, nrows, 60)

        # Checkpoint to ensure that the history store is populated.
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.cc_pages_evict), 0)
        self.assertEqual(self.get_stat(stat.conn.cc_pages_removed), 0)
        self.assertGreater(self.get_stat(stat.conn.cc_pages_visited), 0)

        self.large_updates(uri, bigvalue, ds, nrows, 70)

        # Checkpoint to ensure that the history store is populated.
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.cc_pages_evict), 0)
        self.assertEqual(self.get_stat(stat.conn.cc_pages_removed), 0)
        self.assertGreater(self.get_stat(stat.conn.cc_pages_visited), 0)

if __name__ == '__main__':
    wttest.run()
