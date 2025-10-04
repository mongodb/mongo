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
from wiredtiger import stat
from wtdataset import SimpleDataSet

# test_flcs07.py
#
# Test that implicit records smaller than the greatest record inserted can be found.
class test_flcs07(wttest.WiredTigerTestCase):

    def get_cache_inmem_split(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat.conn.cache_inmem_split][2]
        stat_cursor.close()
        return value

    def test_flcs(self):
        uri = "table:test_flcs07"
        inmem_split = False
        nrows = 1000000
        ds = SimpleDataSet(self, uri, 0, key_format='r', value_format='8t')
        ds.populate()

        # Insert N records with gaps between them.
        gap_size = 10
        cursor = self.session.open_cursor(uri)
        for i in range(10, nrows, gap_size):
            cursor[ds.key(i)] = ds.value(i)
            # As soon as an in-memory split is detected, exit.
            if self.get_cache_inmem_split() > 0:
                inmem_split = True
                break

        assert inmem_split, "At least one in-memory split is expected"

        # Look for every single record smaller than the last one inserted.
        for i in range(i, 1, -1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)

            # Check the value.
            if i % gap_size == 0:
                assert(cursor.get_value() == ds.value(i))
            else:
                assert(cursor.get_value() == 0)

            # FIXME-WT-12682
            # Do a search near on the same key.
            # self.assertEqual(cursor.search_near(), 0)

            # Check the value.
            # if i % gap_size == 0:
            #     assert(cursor.get_value() == ds.value(i))
            # else:
            #     assert(cursor.get_value() == 0)
