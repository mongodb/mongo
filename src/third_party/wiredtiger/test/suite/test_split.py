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
# test_split.py
#       check that splits work as expected
#

import wttest
from wiredtiger import stat

# Test splits
class test_split(wttest.WiredTigerTestCase):
    name = 'test_split'
    conn_config = 'statistics=[all]'
    uri = 'file:' + name

    def test_split_simple(self):
        self.session.create(self.uri,
            'key_format=S,value_format=S,' +
            'allocation_size=4KB,leaf_page_max=4KB,split_pct=75')
        cursor = self.session.open_cursor(self.uri, None)

        # THIS TEST IS DEPENDENT ON THE PAGE SIZES CREATED BY RECONCILIATION.
        # IF IT FAILS, IT MAY BE RECONCILIATION ISN'T CREATING THE SAME SIZE
        # PAGES AS BEFORE.

        # Create a 4KB page (more than 3KB): 40 records w // 10 byte keys
        # and 81 byte values.
        for i in range(35):
            cursor['%09d' % i] = 8 * ('%010d' % i)

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(1, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

        # Now append a few records so we're definitely (a little) over 4KB
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(50,60):
            cursor['%09d' % i] = 8 * ('%010d' % i)

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(2, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

        # Now insert some more records in between
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(40,45):
            cursor['%09d' % i] = 8 * ('%010d' % i)

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(2, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
