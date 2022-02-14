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
from wtdataset import SimpleDataSet

# test_inmem02.py
#    Test in-memory with ignore-cache-size setting.
class test_inmem02(wttest.WiredTigerTestCase):
    uri = 'table:inmem02'
    conn_config = \
        'cache_size=3MB,file_manager=(close_idle_time=0),in_memory=true'
    table_config = 'memory_page_max=32k,leaf_page_max=4k'

    # Add more data than fits into the configured cache and verify it fails.
    def test_insert_over_allowed(self):

        # Create a new table that is allowed to exceed the cache size, do this
        # before filling the cache so that the create succeeds
        self.session.create(
            self.uri + '_over',
            'key_format=S,value_format=S,ignore_in_memory_cache_size=true')

        # Populate a table with enough data to fill the cache.
        msg = '/WT_CACHE_FULL.*/'
        ds = SimpleDataSet(self, self.uri, 10000000, config=self.table_config)
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda:ds.populate(), msg)

        # Add some content to the new table
        cursor = self.session.open_cursor(self.uri + '_over', None)
        for i in range(1, 1000):
            cursor[str('%015d' % i)] = str(i) + ': abcdefghijklmnopqrstuvwxyz'
        cursor.close()

if __name__ == '__main__':
    wttest.run()
