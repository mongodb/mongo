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

# A layered table is dropped while its most recent checkpoint is still ahead of the materialization
# frontier, so its pages cannot leave the cache and the drop hands the constituent handles to the
# sweep server. Sweep must keep retrying those handles rather than retire them with their pages
# still allocated, and must complete the close once the frontier advances.
@disagg_test_class
class test_layered_checkpoint18(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader"),statistics=(all),' + \
        'file_manager=(close_handle_minimum=0,close_idle_time=1,close_scan_interval=1)'

    create_session_config = 'key_format=i,value_format=S'

    uri = f"layered:{test_name}"

    disagg_storages = gen_disagg_storages(disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def wait_until(self, done, message):
        deadline = time.time() + 60
        while not done():
            self.assertLess(time.time(), deadline, 'timed out waiting for ' + message)
            time.sleep(0.5)

    def write_and_checkpoint(self, key, ts):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[key] = 'value'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

    def publish_materialization_frontier(self, page_log):
        (ret, lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)
        page_log.pl_set_last_materialized_lsn(self.session, lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, lsn)

    def test_layered_checkpoint18(self):
        page_log = self.conn.get_page_log(self.vars.page_log)
        self.session.create(self.uri, self.create_session_config)

        self.write_and_checkpoint(1, 1)

        # Publish a materialization frontier at the checkpoint 1 LSN, then write checkpoint 2. The
        # tree is now reconciled ahead of the frontier and its pages cannot leave the cache.
        self.publish_materialization_frontier(page_log)

        self.write_and_checkpoint(2, 2)

        # The drop marks the constituent handles dead and hands them to sweep instead of discarding
        # their pages here.
        self.session.drop(self.uri, 'force=true')

        # Wait for a couple of full sweep passes over the dropped handles.
        target = self.get_stat(wiredtiger.stat.conn.dh_sweeps) + 2
        self.wait_until(lambda: self.get_stat(wiredtiger.stat.conn.dh_sweeps) >= target,
            'the sweep server to visit the dropped handles')

        stuck_pages = self.get_stat(wiredtiger.stat.conn.cache_pages_inuse)

        # Let the frontier catch up. The pages can now be discarded and the handles drain.
        self.publish_materialization_frontier(page_log)

        # Check that sweep can release pages now.
        self.wait_until(
            lambda: self.get_stat(wiredtiger.stat.conn.cache_pages_inuse) < stuck_pages,
            'the dropped tree pages to leave the cache')

        # Connection close is the last chance to free the pages: it reports anything left behind.
        self.close_conn()
        self.captureerr.check(self)
