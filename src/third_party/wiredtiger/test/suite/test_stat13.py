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
from wiredtiger import stat
from wtdataset import SimpleDataSet, simple_key
from wtscenario import make_scenarios

# test_stat13.py
# Check that btree_maximum_depth is computed correctly.

class test_stat13(wttest.WiredTigerTestCase):
    uri = 'table:test_stat13'

    keyfmt = [
        ('column', dict(keyfmt='r', valfmt='S')),
        ('column-fix', dict(keyfmt='r', valfmt='8t')),
        ('string-row', dict(keyfmt='S', valfmt='S')),
    ]
    scenarios = make_scenarios(keyfmt)

    conn_config = 'statistics=(all)'

    def check_depth(self, expect):
        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None, None)
        depth = stat_cursor[stat.dsrc.btree_maximum_depth][2]
        self.assertEqual(depth, expect)
        stat_cursor.close()

    def test_btree_depth(self):
        # Populate a table with a few records. This will create a two-level tree with a root
        # page and one or more leaf pages. We aren't inserting nearly enough records to need
        # an additional level
        nentries = 100
        ds = SimpleDataSet(self, self.uri, nentries,
            key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()
        self.session.checkpoint()

        # Confirm that tree has expected depth
        self.check_depth(2)

        # Check that we have the same result on a new connection.
        self.reopen_conn()

        # The btree_maximum_depth statistic tracks the maximum depth seen on a table. So we
        # have to perform an operation on the table to know its depth.
        cursor = ds.open_cursor()
        cursor.set_key(ds.key(50))
        self.assertEqual(cursor.search(), 0)
        cursor.close()

        self.check_depth(2)
