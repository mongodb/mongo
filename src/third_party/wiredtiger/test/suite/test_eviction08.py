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

import time
import wttest
from wiredtiger import stat

# A table that dominates the cache can supply every eviction candidate the server asks for, pass
# after pass. The eviction scan must still rotate to other tables once it has fully traversed that
# table, rather than walking it over and over while other tables are never visited.
@wttest.skip_for_hook("disagg", "Layout and eviction behavior differ under disaggregated storage.")
@wttest.skip_for_hook("tiered", "Layout and eviction behavior differ under tiered storage.")
class test_eviction08(wttest.WiredTigerTestCase):
    conn_config = ('cache_size=20MB,statistics=(all),'
                   'eviction=(threads_min=1,threads_max=1)')
    table_config = 'key_format=i,value_format=S,leaf_page_max=4KB,memory_page_max=512KB'

    dominant_uri = 'table:test_eviction08_dominant'
    small_uris = [f'table:test_eviction08_small{i}' for i in range(8)]
    dominant_rows = 40000
    small_rows = 50
    value = 'abcde' * 100

    def test_eviction_walk_rotation(self):
        for uri in [self.dominant_uri] + self.small_uris:
            self.session.create(uri, self.table_config)

        # Fill the dominant table well past the cache size.
        cursor = self.session.open_cursor(self.dominant_uri)
        for i in range(self.dominant_rows):
            if i % 1000 == 0:
                self.session.begin_transaction()
            cursor[i] = self.value
            if i % 1000 == 999:
                self.session.commit_transaction()
        cursor.close()

        small_cursors = []
        for uri in self.small_uris:
            c = self.session.open_cursor(uri)
            for i in range(self.small_rows):
                c[i] = self.value
            small_cursors.append(c)

        # Keep the dominant table full of eviction candidates and the small tables in cache
        # until the scan reports moving past a fully traversed tree. Fail as a timeout if it
        # never does.
        cursor = self.session.open_cursor(self.dominant_uri)
        deadline = time.time() + 120
        offset = 0
        while self.get_stat(stat.conn.eviction_server_skip_trees_walk_complete) == 0:
            self.assertLess(time.time(), deadline,
                'eviction scan never rotated off the dominant table')
            offset = (offset + 1) % 37
            self.session.begin_transaction()
            for i in range(offset, self.dominant_rows, 37):
                cursor[i] = self.value
            self.session.commit_transaction()
            for c in small_cursors:
                for i in range(self.small_rows):
                    c[i] = self.value

        cursor.close()
        for c in small_cursors:
            c.close()
