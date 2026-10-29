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

import time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Verify a table on a follower that never set a stable timestamp, after the leader wrote a leaf as a
# base image followed by one delta. The delta is written either by the checkpoint or by an earlier
# eviction of the dirty leaf; full page writes are the control.
@disagg_test_class
class test_verify_disagg07(wttest.WiredTigerTestCase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    page_formats = [
        ('full', dict(write_delta=False)),
        ('delta', dict(write_delta=True)),
    ]
    writers = [
        ('ckpt_write', dict(evict_first=False)),
        ('evict_write', dict(evict_first=True)),
    ]
    scenarios = make_scenarios(disagg_storages, page_formats, writers)

    conn_config_follower = 'disaggregated=(role="follower"),statistics=(all)'

    uri = f'layered:{test_name}'
    table_cfg = 'key_format=S,value_format=S,block_manager=disagg,leaf_page_max=128KB'

    def conn_config(self):
        delta_config = ('delta_pct=100,leaf_page_delta=true' if self.write_delta else
                        'delta_pct=1,leaf_page_delta=false')
        return ('disaggregated=(role="leader"),statistics=(all),page_delta=(' +
                delta_config + ',internal_page_delta=false)')

    def leaf_delta_count(self):
        stats = self.session.open_cursor('statistics:' + self.uri)
        count = stats[wiredtiger.stat.dsrc.rec_page_delta_leaf][2]
        stats.close()
        return count

    # An eviction attempt can fail on a busy page and leave it dirty, so retry until the delta is
    # written.
    def evict_leaf(self):
        for _ in range(50):
            evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict=true)')
            evict_cursor.set_key('key00')
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
            evict_cursor.close()
            if not self.write_delta or self.leaf_delta_count() > 0:
                return
            time.sleep(0.1)
        self.fail('the dirty leaf was not evicted as a delta')

    def test_verify_delta_empty_parent_without_follower_stable(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.table_cfg)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(20):
            cursor[f'key{i:02d}'] = 'a' * 200
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        self.session.begin_transaction()
        cursor['key00'] = 'b' * 200
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
                                ',stable_timestamp=' + self.timestamp_str(10))
        cursor.close()
        if self.evict_first:
            self.evict_leaf()
        self.session.checkpoint()

        if self.write_delta:
            self.assertGreater(self.leaf_delta_count(), 0)
        else:
            self.assertEqual(self.leaf_delta_count(), 0)
        self.verifyUntilSuccess(self.session)

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                           self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        try:
            self.disagg_advance_checkpoint_and_wait(conn_follow)
            self.assertEqual(conn_follow.query_timestamp('get=stable_timestamp'), '0')
            follower_cursor = session_follow.open_cursor(self.uri)
            self.assertEqual(follower_cursor['key00'], 'b' * 200)
            self.assertEqual(follower_cursor['key01'], 'a' * 200)
            follower_cursor.close()
            self.verifyUntilSuccess(session_follow)
        finally:
            session_follow.close()
            conn_follow.close()
