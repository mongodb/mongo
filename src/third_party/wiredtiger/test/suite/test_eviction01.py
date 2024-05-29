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

# test_eviction01.py
'''
This test inserts (then rolls back) a large number of updates per key in the update chain to a
point where only aborted updates are present in the chain. We then test whether these chains,
filled with aborted updates, get evicted successfully.
'''
class test_eviction01(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=1GB'
    nrows = 100
    iterations = 500

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_eviction(self):
        uri = "table:test_eviction01"
        ds = SimpleDataSet(self, uri, self.nrows, key_format='S', value_format='u')
        ds.populate()

        cursor = self.session.open_cursor(uri)
        for _ in range(1, self.iterations):
            self.session.begin_transaction()
            for i in range(1, self.nrows):
                cursor[ds.key(i)] = b"aaaaa" * 100
            self.session.rollback_transaction()
        cursor.close()

        self.assertGreater(self.get_stat(stat.conn.cache_eviction_dirty), 0)
        self.assertEqual(self.get_stat(stat.conn.cache_eviction_blocked_no_progress), 0)
