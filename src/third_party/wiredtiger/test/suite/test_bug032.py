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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_bug032.py
# This tests for the scenario discovered in WT-11845.
# Before WT-11845 fast truncate determined if a page could be fast truncated by
# looking at the pages aggregated timestamp. This would lead to keys being incorrectly
# truncated if:
# - Two transactions txn1 and txn2 have ids 10 and 20 respectively
# - txn2 is committed and txn1 is still active while a truncate transaction begins.
#   txn2 is visible to the truncate while txn1 is not.
# - txn1 is then committed and a page containing updates from both transactions is written to disk.
#   The aggregated timestamp uses txn id 20.
# - Truncate reads the aggregated timestamp of the page (id 20) and determines it is visible.
#   The page is truncated even though it contains updates from txn1 which is not visible to the
#   truncate operation.
class test_bug032(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('string_row', dict(key_format='S', value_format='S')),
    ]
    scenarios = make_scenarios(format_values)

    def populate(self, uri, ds, nrows, value):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            cursor[ds.key(i)] = value + str(i)

        self.session.commit_transaction()
        cursor.close()

    def test_bug032(self):
        uri = 'table:bug032'
        nrows = 500

        # The key to be inserted and truncated in parallel, as described in the test description.
        txn1_key_num = 32

        # 512 byte keys. We're limiting leaf_page_max to 10KB which will give us ~20 keys per page.
        value_str = 'a' * 512

        # Create a table and populate it
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format, config='allocation_size=512,leaf_page_max=10KB')
        ds.create()
        self.populate(uri, ds, nrows, value_str)

        # Remove txn1's key which was added by the populate phase. We want truncate and insert to race in
        # a way they aren't aware of each other. If there's an existing version of the key then txn1's
        # insert becomes a modify and truncate will see the parallel modification, rolling back instead
        # of truncating.
        self.session.begin_transaction()
        c = self.session.open_cursor(uri)
        c.set_key(ds.key(txn1_key_num))
        c.remove()
        self.session.commit_transaction()
        c.close()

        # Perform an in txn1 but don't commit it.
        txn1_session = self.conn.open_session()
        txn1_session.begin_transaction()
        c1 = txn1_session.open_cursor(uri)
        c1[ds.key(txn1_key_num)] = value_str
        c1.reset()

        # Start and commit txn2. This has a larger txn id than txn1.
        txn2_session = self.conn.open_session()
        txn2_session.begin_transaction()
        c2 = txn2_session.open_cursor(uri)
        c2[ds.key(txn1_key_num + 1)] = value_str
        txn2_session.commit_transaction()
        c2.close()

        # Start the truncate transaction. Here the snapshot will see txn2 but not txn1.
        truncate_session = self.conn.open_session()
        truncate_session.begin_transaction()

        # Commit txn1.
        txn1_session.commit_transaction()
        c1.reset()
        c1.close()

        # Evict the page txn1 containing modifications from both txn1 and txn2.
        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        evict_cursor[ds.key(txn1_key_num)]
        self.assertEqual(evict_cursor.reset(), 0)
        evict_cursor.close()

        # Truncate everything. This will attempt to fast truncate the page we evicted to disk.
        # The page cannot be fast truncated as txn1's key is not visible to the truncate.
        truncate_session.truncate(uri, None, None, None)
        truncate_session.commit_transaction()

        # Search for our key inserted by txn1. This was not truncated by the truncate operation as
        # txn1 wasn't visible.
        validate_cursor = self.session.open_cursor(ds.uri)
        validate_cursor.set_key(ds.key(txn1_key_num))
        self.assertEqual(validate_cursor.search(), 0)
