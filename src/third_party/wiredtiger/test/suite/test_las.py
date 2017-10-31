#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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
from wtdataset import SimpleDataSet

# test_las.py
#       Smoke tests to ensure lookaside tables are working.
class test_las(wttest.WiredTigerTestCase):
    # Force a small cache.
    def conn_config(self):
        return 'cache_size=1GB'

    @wttest.longtest('lookaside table smoke test')
    def test_las(self):
        # Create a small table.
        uri = "table:test_las"
        nrows = 100
        ds = SimpleDataSet(self, uri, nrows, key_format="S")
        ds.populate()

        # Take a snapshot.
        self.session.snapshot("name=xxx")

        # Insert a large number of records, we'll hang if the lookaside table
        # isn't doing its thing.
        c = self.session.open_cursor(uri)
        bigvalue = "abcde" * 100
        for i in range(1, 1000000):
            c.set_key(ds.key(nrows + i))
            c.set_value(bigvalue)
            self.assertEquals(c.insert(), 0)

if __name__ == '__main__':
    wttest.run()
