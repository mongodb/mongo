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

import wiredtiger, wttest
from helper import WiredTigerCursor
from helper_disagg import disagg_test_class
from sweep_util import sweep_util

# On step-up the shared disk cache goes read-only; after the sweep server's
# grace period it goes dead, blocking GET entirely.
@disagg_test_class
class test_layered_stepup11(sweep_util):
    nrows = 5000

    conn_base_config = 'statistics=(all),' \
                     + 'disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_follow_config = conn_base_config \
        + 'file_manager=(close_scan_interval=1),' \
        + 'disaggregated=(role="follower")'

    # Small pages so the tree has many leaves; cap the in-memory page size so
    # pages don't absorb extra rows during reconciliation.
    create_session_config = 'key_format=S,value_format=S,' \
        + 'allocation_size=512,leaf_page_max=512,internal_page_max=512,memory_page_max=512'

    uri = 'layered:test_layered_stepup11'

    def key(self, i):
        return str(i).zfill(5)

    def scan_cache_diff(self, session, limit=None):
        with WiredTigerCursor(session, 'statistics:') as stat_cursor:
            miss_before = stat_cursor[wiredtiger.stat.conn.cache_shared_dsk_miss][2]
            hit_before = stat_cursor[wiredtiger.stat.conn.cache_shared_dsk_hit][2]

        with WiredTigerCursor(session, self.uri) as cursor:
            count = 0
            ret = 0
            while ret == 0:
                ret = cursor.next()
                if ret == 0:
                    count += 1
                if limit is not None and count >= limit:
                    break
            if limit is None:
                # Full scan must reach end of records cleanly.
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                self.assertEqual(count, self.nrows)
            else:
                # Partial scan stops mid-table, so the last cursor.next() succeeded.
                self.assertEqual(ret, 0)

        with WiredTigerCursor(session, 'statistics:') as stat_cursor:
            miss = stat_cursor[wiredtiger.stat.conn.cache_shared_dsk_miss][2] - miss_before
            hit = stat_cursor[wiredtiger.stat.conn.cache_shared_dsk_hit][2] - hit_before
        return miss, hit

    def update_one_and_checkpoint(self, session, value):
        cursor = session.open_cursor(self.uri)
        cursor[self.key(0)] = value
        cursor.close()
        session.checkpoint()

    def test_layered_stepup11(self):
        self.ignoreStdoutPattern('Removing local file due to disagg mode')

        self.session.create(self.uri, self.create_session_config)

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,'
            + self.conn_follow_config)
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.create_session_config)

        # Leader bulk loads and checkpoints so conn_follow can read the data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[self.key(i)] = 'value_' + str(i)
        cursor.close()
        self.session.checkpoint()

        # Follower scans to populate its shared disk cache while ACTIVE.
        self.disagg_advance_checkpoint(conn_follow)
        self.scan_cache_diff(session_follow)

        # One more checkpoint the follower does not pick up as its root-to-leaf
        # path will be cold when conn_follow steps up.
        self.update_one_and_checkpoint(self.session, 'nocache')

        self.disagg_switch_follower_and_leader(conn_follow, self.conn)

        #
        # READONLY: a half scan confirms reads still work after step-up.
        # Scanning only half the rows leaves the second half cold in the buffer
        # pool as those cold pages are what make the DEAD check meaningful below.
        #
        miss, hit = self.scan_cache_diff(session_follow, limit=self.nrows // 2)
        self.assertGreater(miss, 0)
        self.assertGreater(hit, 0)

        #
        # DEAD: wait for the sweep server to mark the cache dead (grace period
        # is 5 seconds, close_scan_interval=1 ensures the sweep ticks in time).
        # A full scan is used so the second half of the table, which was not
        # scanned above, is reached with a cold buffer pool. DEAD blocks GET
        # entirely so those cold pages produce neither a hit nor a miss,
        # proving the cache is fully bypassed and not just buffer-pool-warm.
        #
        self.wait_for_sweep(increment=7, timeout=60, poll_interval=1, session=session_follow)
        miss, hit = self.scan_cache_diff(session_follow)
        self.assertEqual(hit, 0)
        self.assertEqual(miss, 0)

        session_follow.close()
        conn_follow.close()
