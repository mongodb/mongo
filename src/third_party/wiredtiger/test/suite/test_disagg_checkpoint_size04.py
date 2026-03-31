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

import re, wttest
from helper_disagg import disagg_test_class

# test_disagg_checkpoint_size04.py
#    Test that dropping a table reduces the database size in disaggregated storage.
@disagg_test_class
class test_disagg_checkpoint_size04(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'

    def get_database_size(self):
        match = re.search(r'database_size=(\d+)', self.disagg_get_complete_checkpoint_meta())
        assert(match)
        return int(match.group(1))

    def test_drop_reduces_database_size(self):
        uri = "layered:test_table"
        self.session.create(uri, 'key_format=i,value_format=S')

        self.session.checkpoint()
        size_empty = self.get_database_size()

        cursor = self.session.open_cursor(uri)
        for i in range(1000):
            cursor[i] = 'a' * 500
        cursor.close()

        self.session.checkpoint()
        size_with_data = self.get_database_size()

        data_size = size_with_data - size_empty
        self.assertGreater(data_size, 0,
            f"Data insertion should increase size: {size_empty} -> {size_with_data}")

        self.session.drop(uri)

        # The drop is queued; it takes effect in the next checkpoint.
        self.session.checkpoint()
        size_after_drop = self.get_database_size()

        self.pr(f"empty={size_empty}, with_data={size_with_data}, after_drop={size_after_drop}")
        self.assertLess(size_after_drop, size_with_data,
            f"Database size should decrease after drop: {size_with_data} -> {size_after_drop}")

        # The size after dropping should be close to the empty-table size. Allow some slack for
        # shared metadata overhead changes but the bulk of the data should be gone.
        self.assertLess(size_after_drop, size_empty + data_size * 0.1,
            f"Most of the data should be reclaimed: empty={size_empty}, "
            f"after_drop={size_after_drop}, data_size={data_size}")

    def test_drop_one_of_multiple_tables(self):
        uri1 = "layered:test_keep"
        uri2 = "layered:test_drop"
        self.session.create(uri1, 'key_format=i,value_format=S')
        self.session.create(uri2, 'key_format=i,value_format=S')

        self.session.checkpoint()
        size_empty = self.get_database_size()

        # Insert roughly equal amounts of data into both tables.
        for uri in [uri1, uri2]:
            cursor = self.session.open_cursor(uri)
            for i in range(1000):
                cursor[i] = 'a' * 500
            cursor.close()

        self.session.checkpoint()
        size_both = self.get_database_size()
        total_data = size_both - size_empty
        self.assertGreater(total_data, 0)

        # Drop only the second table.
        self.session.drop(uri2)
        self.session.checkpoint()
        size_after_drop = self.get_database_size()

        self.pr(f"empty={size_empty}, both={size_both}, after_drop={size_after_drop}, "
                f"total_data={total_data}")

        # The size should decrease by roughly half the data (the dropped table's share).
        removed = size_both - size_after_drop
        self.assertGreater(removed, total_data * 0.3,
            f"Drop should reclaim a significant portion: removed={removed}, total_data={total_data}")

        # The surviving table's data should still be accounted for.
        surviving_data = size_after_drop - size_empty
        self.assertGreater(surviving_data, total_data * 0.3,
            f"Surviving table data should remain: surviving={surviving_data}, total_data={total_data}")
