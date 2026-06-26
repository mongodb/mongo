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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# A follower scanning a checkpoint caches every disk image it reads. After the
# leader modifies one row and takes another checkpoint, the follower's scan of
# the new checkpoint only misses on the modified root-to-leaf path and hits on
# every other page.
@disagg_test_class
class test_cross_checkpoint_caching(wttest.WiredTigerTestCase):
    test_name = __qualname__

    conn_base_config = ',create,statistics=(all),'

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    uri = f'layered:{test_name}'
    nrows = 5000

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Scan the table on the follower, returning the scan's miss and hit deltas
    # so reads done outside the scan don't count.
    def follower_scan(self, session_follow):
        miss_before = self.get_stat(wiredtiger.stat.conn.cache_shared_dsk_miss, session=session_follow)
        hit_before = self.get_stat(wiredtiger.stat.conn.cache_shared_dsk_hit, session=session_follow)
        cursor = session_follow.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)
        miss = self.get_stat(wiredtiger.stat.conn.cache_shared_dsk_miss, session=session_follow) \
            - miss_before
        hit = self.get_stat(wiredtiger.stat.conn.cache_shared_dsk_hit, session=session_follow) \
            - hit_before
        return miss, hit

    def test_cross_checkpoint_caching(self):
        # Small pages so the tree has internal levels and many leaves; cap the
        # in-memory page size to prevent pages from growing large enough to
        # absorb extra rows during reconciliation.
        table_cfg = 'key_format=S,value_format=S,' \
            'allocation_size=512,leaf_page_max=512,internal_page_max=512,memory_page_max=512'
        self.session.create(self.uri, table_cfg)
        conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, table_cfg)

        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[str(i).zfill(5)] = 'value_' + str(i)
        cursor.close()
        self.session.checkpoint()

        # The follower's first scan caches every read from disk.
        self.disagg_advance_checkpoint(conn_follow)
        first_miss, first_hit = self.follower_scan(session_follow)
        self.assertGreater(first_miss, 1)
        self.assertEqual(first_hit, 0)

        # Update one row, keeping the value size unchanged so no page splits.
        # The next checkpoint modifies only that row's root-to-leaf path.
        cursor = self.session.open_cursor(self.uri)
        cursor[str(0).zfill(5)] = 'updated'
        cursor.close()
        self.session.checkpoint()

        # Scanning the new checkpoint misses on the rewritten path and hits on
        # every other page.
        self.disagg_advance_checkpoint(conn_follow)
        second_miss, second_hit = self.follower_scan(session_follow)
        self.assertGreater(second_miss, 0)
        self.assertGreater(second_hit, 0)
        self.assertEqual(second_hit, first_miss - second_miss)

        session_follow.close()
        conn_follow.close()
