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

import random, wttest
from wtdataset import SimpleDataSet

# test_bug011.py
#    Eviction working on more trees than the eviction server can walk
#    simultaneously.  There is a builtin limit of 1000 trees, we open double
#    that, which makes this a long-running test.
class test_bug011(wttest.WiredTigerTestCase):
    """
    Test having eviction working on more files than the number of
    allocated hazard pointers.
    """
    table_name = 'test_bug011'
    ntables = 2000
    nrows = 10000
    nops = 10000
    # Add connection configuration for this test.
    def conn_config(self):
        return 'cache_size=1GB'

    @wttest.longtest("Eviction copes with lots of files")
    def test_eviction(self):
        cursors = []
        datasets = []
        for i in range(0, self.ntables):
            this_uri = 'table:%s-%05d' % (self.table_name, i)
            ds = SimpleDataSet(self, this_uri, self.nrows,
                               config='allocation_size=1KB,leaf_page_max=1KB')
            ds.populate()
            datasets.append(ds)

        # Switch over to on-disk trees with multiple leaf pages
        self.reopen_conn()

        # Make sure we have a cursor for every table so it stays in cache.
        for i in range(0, self.ntables):
            this_uri = 'table:%s-%05d' % (self.table_name, i)
            cursors.append(self.session.open_cursor(this_uri, None))

        # Make use of the cache.
        for i in range(0, self.nops):
            for i in range(0, self.ntables):
                cursors[i].set_key(ds.key(random.randint(0, self.nrows - 1)))
                cursors[i].search()
                cursors[i].reset()

if __name__ == '__main__':
    wttest.run()
