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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# Test no page delta is generated on page split.

@disagg_test_class
class test_layered_delta10(wttest.WiredTigerTestCase):
    test_name = __qualname__
    split = [
        ('page_split', dict(page_split=True)),
        ('page_no_split', dict(page_split=False)),
    ]

    conn_config = 'cache_size=10MB,transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(role="leader"),page_delta=(delta_pct=100,internal_page_delta=true,leaf_page_delta=true)'
    disagg_storages = gen_disagg_storages(disagg_only = True)

    uri=f'layered:{test_name}'

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages, split)

    def test_page_split_delta(self):
        self.session.create(self.uri,
            'key_format=S,value_format=S,block_manager=disagg,' +
            'allocation_size=4KB,leaf_page_max=4KB,split_pct=75')
        cursor = self.session.open_cursor(self.uri, None)

        # THIS TEST IS DEPENDENT ON THE PAGE SIZES CREATED BY RECONCILIATION.
        # IF IT FAILS, IT MAY BE RECONCILIATION ISN'T CREATING THE SAME SIZE
        # PAGES AS BEFORE.

        # Create a 4KB page (more than 3KB): 34 records with 10-byte keys and 81-byte values.
        # Each record takes 99 bytes on the page (10-byte key cell + 81-byte value cell + 8-byte
        # commit timestamp), 34 x 99 + 44 (page header) = 3410 bytes. The timestamps also keep
        # the update chains non-globally-visible (oldest is never set), and the 35th record's
        # retained chain tips reconciliation's size accounting past the split point.
        for i in range(34):
            self.session.begin_transaction()
            cursor['%09d' % i] = 8 * ('%010d' % i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))

        # Stabilize
        self.reopen_conn()
        # We should only have one leaf page.
        self.assertEqual(self.get_stat(stat.dsrc.btree_row_leaf, self.uri), 1)

        if self.page_split:
            # Make an update so we can later check that page split will not generate delta.
            cursor = self.session.open_cursor(self.uri, None)
            self.session.begin_transaction()
            cursor['%09d' % 30] = 8 * ('%010d' % 31)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(35))
            # Append a few records so we're definitely (a little) over 4KB.
            for i in range(50,60):
                self.session.begin_transaction()
                cursor['%09d' % i] = 8 * ('%010d' % i)
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i - 50 + 36))
            cursor.close()

            self.session.checkpoint()
            # No delta is created.
            self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 0)

            # Reopen the connection and valid a page split has happened.
            self.reopen_conn()
            self.assertEqual(self.get_stat(stat.dsrc.btree_row_leaf, self.uri), 2)
        else:
            # Make an update so we can later check that a delta has been generated.
            cursor = self.session.open_cursor(self.uri, None)
            self.session.begin_transaction()
            cursor['%09d' % 30] = 8 * ('%010d' % 31)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(35))
            cursor.close()

            self.session.checkpoint()
            # We created one delta.
            self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 1)

            # Reopen the connection and validate that a page split did not occur.
            self.reopen_conn()
            self.assertEqual(self.get_stat(stat.dsrc.btree_row_leaf, self.uri), 1)
