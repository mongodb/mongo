#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
# test_cursor15.py
#   Cursors: read_once configuration
#

import wttest
from wiredtiger import stat

class test_cursor15(wttest.WiredTigerTestCase):
    tablename = 'test_read_once'
    uri = 'table:' + tablename

    conn_config = 'cache_size=1M,statistics=(all)'

    def test_cursor15(self):
        # This test is configured to use 1MB of cache. It will insert 20
        # documents, each 100KB. Manipulate the table to result in one page per
        # document.
        self.session.create(self.uri,
            'key_format=i,value_format=S,leaf_page_max=108K,leaf_value_max=108K')

        cursor = self.session.open_cursor(self.uri, None, None)
        for key in range(0, 20):
            cursor[key] = '1' * (100 * 1024)
        cursor.close()

        # Restart the database to clear the cache and reset statistics.
        self.reopen_conn()

        # We don't restart the database between runs to exercise that read_once
        # plays nice with cursor caching. Note, there are no reliable statistics
        # to check that read_once behaves as expected. The future may introduce a
        # statistic for WT_READ_WONT_NEED being exercised. That may work as a
        # suitable side-effect to observe here.
        for cursor_conf in ["read_once=true", None]:
            # Table scan ~2MB of data when only given 1MB of cache.
            cursor = self.session.open_cursor(self.uri, None, cursor_conf)
            for key, value in cursor:
                pass
            cursor.close()

if __name__ == '__main__':
    wttest.run()
