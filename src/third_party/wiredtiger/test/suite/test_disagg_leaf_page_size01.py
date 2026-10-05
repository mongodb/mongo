#!/usr/bin/env python3
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
from helper_disagg import disagg_test_class
from wiredtiger import stat

# An append-only load on a disaggregated table builds leaf pages of the same size as an identical
# local table.
@disagg_test_class
class test_disagg_leaf_page_size01(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true),statistics=(fast),' \
        'cache_size=1GB'
    nrows = 40000
    table_config = 'key_format=S,value_format=S,leaf_page_max=32KB,split_pct=90'
    local_uri = 'table:leaf_size_local'
    shared_uri = 'table:leaf_size_shared'

    def populate(self, uri, config, ts):
        self.session.create(uri, config)
        with wttest.open_cursor(self.session, uri) as cursor:
            for i in range(self.nrows):
                with self.transaction(commit_timestamp=ts):
                    cursor[f'key{i:010d}'] = 'x' * 1000
                ts += 1
        return ts

    def leaf_pages(self, uri):
        """Walk the tree and return its number of leaf pages."""
        with wttest.open_cursor(self.session, uri, config='debug=(size_stats)') as cursor:
            while cursor.next() == 0:
                pass
        with wttest.open_cursor(self.session, f'statistics:{uri}',
                                config='statistics=(fast)') as cursor:
            return cursor[stat.dsrc.btree_size_leaf_pages][2]

    def test_page_count_matches_local(self):
        ts = self.populate(self.local_uri, self.table_config + ',block_manager=default,type=file', 1)
        ts = self.populate(self.shared_uri, self.table_config + ',block_manager=disagg,type=file', ts)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

        # Reopen without wiping the page store, so every page is read back from its checkpoint.
        self.conn_config = self.conn_config.replace(',lose_all_my_data=true', '')
        self.reopen_conn()

        local_pages = self.leaf_pages(self.local_uri)
        shared_pages = self.leaf_pages(self.shared_uri)
        self.assertGreater(local_pages, 0)
        # The same data must not need noticeably more pages when stored on the disaggregated block
        # manager.
        self.assertLessEqual(shared_pages, local_pages * 1.1)
