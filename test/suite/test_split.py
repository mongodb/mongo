#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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

import wiredtiger, wttest
from wiredtiger import stat
from helper import confirm_empty,\
    key_populate, value_populate, simple_populate,\
    complex_populate, complex_value_populate
from wtscenario import multiply_scenarios, number_scenarios

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

        # Create a 4KB page (more than 3KB): 40 records w / 10 byte keys
        # and 81 byte values.
        for i in range(40):
            cursor.set_key('%09d' % i)
            cursor.set_value(8 * ('%010d' % i))
            cursor.insert()

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(1, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

        # Now append a few records so we're definitely (a little) over 4KB
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(50,55):
            cursor.set_key('%09d' % i)
            cursor.set_value(8 * ('%010d' % i))
            cursor.insert()

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(2, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

        # Now insert some more records in between
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(40,45):
            cursor.set_key('%09d' % i)
            cursor.set_value(8 * ('%010d' % i))
            cursor.insert()

        # Stabilize
        self.reopen_conn()

        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None)
        self.assertEqual(2, stat_cursor[stat.dsrc.btree_row_leaf][2])
        stat_cursor.close()

    def test_split_prefix(self):
        this_uri = self.uri + 'prefix'
        # Configure 4KB pages with prefix compression enabled and support for
        # large data items.
        self.session.create(this_uri,
                'prefix_compression=1,' +
                'key_format=S,value_format=S,' +
                'internal_page_max=4KB,leaf_page_max=4KB,' +
                'leaf_value_max=3096')

        cursor = self.session.open_cursor(this_uri, None)
        # Insert two items with keys that will be prefix compressed and data
        # items sized so that the compression size difference tips the
        # size over a page boundary.
        cursor.set_key('fill_2__b_27')
        cursor.set_value(2294 * '0')
        cursor.insert()

        cursor.set_key('fill_2__b_28')
        cursor.set_value(3022 * '0')
        cursor.insert()

if __name__ == '__main__':
    wttest.run()
