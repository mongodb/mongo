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

# Verify that a scan of a delete-heavy stable btree after step-down can dirty the outdated tree,
# and that the dirty pages are discarded once the scan's cursor is closed.

import time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_stepdown_dirty_outdated(wttest.WiredTigerTestCase):
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:test_layered_stepdown_dirty_outdated'
    stable_uri = 'file:test_layered_stepdown_dirty_outdated.wt_stable'

    # Half the keys are deleted: the deleted-key heuristic needs strictly more than
    # WT_BTREE_DELETE_THRESHOLD (1000) tombstones on a single page pass to fire.
    nitems = 3000

    def conn_config(self):
        return self.extensionsConfig() + ',create,cache_size=512MB,statistics=(all),' + \
            'file_manager=(close_scan_interval=1,close_idle_time=1),' + \
            'disaggregated=(lose_all_my_data=true,role="leader")'

    def get_stat(self, stat):
        cursor = self.session.open_cursor('statistics:')
        value = cursor[stat][2]
        cursor.close()
        return value

    def test_scan_dirties_outdated_tree(self):
        self.session.create(self.uri, 'key_format=S,value_format=S,leaf_page_max=4KB')

        cursor = self.session.open_cursor(self.stable_uri)
        self.session.begin_transaction()
        for i in range(self.nitems):
            cursor[f'key{i:06d}'] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        self.session.begin_transaction()
        for i in range(0, self.nitems, 2):
            cursor.set_key(f'key{i:06d}')
            cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # The heuristic only counts tombstones visible to all readers, so advance oldest past the
        # deletes. The oldest transaction ID only advances lazily, so a checkpoint forces it; the
        # tombstones survive reconciliation because they are not yet globally visible then.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20) +
                                ',oldest_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        self.assertEqual(self.get_stat(wiredtiger.stat.conn.cache_bytes_dirty), 0)

        # Keep a cursor on the live stable tree across the step-down and scan it afterwards.
        scan_session = self.conn.open_session('')
        scan_cursor = scan_session.open_cursor(self.stable_uri)
        self.conn.reconfigure('disaggregated=(role="follower")')
        while scan_cursor.next() == 0:
            pass
        self.assertGreater(self.get_stat(wiredtiger.stat.conn.eviction_force_delete), 0)
        scan_cursor.close()
        scan_session.close()

        # Sweep closes the outdated handle and discards its dirty pages without writing them.
        deadline = time.time() + 60
        while self.get_stat(wiredtiger.stat.conn.cache_bytes_dirty) != 0:
            self.assertLess(time.time(), deadline, 'outdated tree was not discarded')
            time.sleep(0.5)
