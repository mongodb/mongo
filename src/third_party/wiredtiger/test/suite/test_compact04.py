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
# test_compact04.py
#   Test the accuracy of compact work estimation.
#

import time, wiredtiger, wttest
from wiredtiger import stat

# Test the accuracy of compact work estimation.
class test_compact04(wttest.WiredTigerTestCase):

    conn_config = 'statistics=(all),cache_size=100MB'
    # Uncomment for debugging:
    # conn_config += ',verbose=(compact_progress,compact:4)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    table_numkv = 100 * 1000
    table_uri='table:test_compact04'

    delete_range_len = 10 * 1000
    delete_ranges_count = 4
    value_size = 1024 # The value should be small enough so that we don't create overflow pages.

    # Create the table in a way that it creates a mostly empty file.
    def test_compact04(self):

        # Create the table and populate it with a lot of data
        self.session.create(self.table_uri, self.create_params)
        c = self.session.open_cursor(self.table_uri, None)
        for k in range(self.table_numkv):
            c[k] = ('%07d' % k) + '_' + 'abcd' * ((self.value_size // 4) - 2)
        c.close()
        self.session.checkpoint()

        # Now let's delete a lot of data ranges. Create enough space so that compact runs in more
        # than one iteration.
        c = self.session.open_cursor(self.table_uri, None)
        for r in range(self.delete_ranges_count):
            start = r * self.table_numkv // self.delete_ranges_count
            for i in range(self.delete_range_len):
                c.set_key(start + i)
                c.remove()
        c.close()

        # Compact!
        self.session.compact(self.table_uri, None)
        self.ignoreStdoutPatternIfExists('WT_VERB_COMPACT')
        self.ignoreStderrPatternIfExists('WT_VERB_COMPACT')

        # Verify the compact progress stats.
        c_stat = self.session.open_cursor('statistics:' + self.table_uri, None, 'statistics=(all)')
        pages_rewritten = c_stat[stat.dsrc.btree_compact_pages_rewritten][2]
        pages_rewritten_expected = c_stat[stat.dsrc.btree_compact_pages_rewritten_expected][2]
        c_stat.close()

        self.assertGreater(pages_rewritten, 0)
        self.assertGreater(pages_rewritten_expected, 0)

        d = abs(pages_rewritten - pages_rewritten_expected) / pages_rewritten
        self.assertLess(d, 0.15)

if __name__ == '__main__':
    wttest.run()
