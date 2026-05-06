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

# test_layered100.py
#   Verify that fast truncate prevents internal page delta writes.
#   Phase 2 proves delta fires for a normal update (sanity); Phase 3 proves it
#   is suppressed when reconciliation encounters a WT_REF_DELETED child.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered100(wttest.WiredTigerTestCase):
    uri         = 'layered:test_layered100'
    nrows       = 200
    value       = 'a' * 50
    trunc_start = 50
    trunc_stop  = 150

    # Small pages produce ~25 leaves and multiple non-root internal pages (3-level tree);
    # only non-root internals are eligible for WT_BUILD_DELTA_INT.
    page_cfg    = 'allocation_size=512,leaf_page_max=512,internal_page_max=512'

    conn_base_config = 'cache_size=50MB,statistics=(all),' \
                       'page_delta=(internal_page_delta=true,leaf_page_delta=false,delta_pct=100),'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    disagg_storages = gen_disagg_storages('test_layered100', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def get_stat(self, conn, stat_key, uri=None):
        s = conn.open_session('')
        val = s.open_cursor('statistics:' + (uri or ''))[stat_key][2]
        s.close()
        return val

    def leader_checkpoint(self, ts):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts) +
                                ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def setup_leader(self, extra_cfg=''):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=i,value_format=S' + extra_cfg)
        cur = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cur[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cur.close()
        self.leader_checkpoint(10)

        # Evict all leaves to WT_REF_DISK, satisfying the first fast delete eligibility condition.
        evict_cur = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            evict_cur.set_key(i)
            evict_cur.search()
            evict_cur.reset()
        evict_cur.close()
        self.session.rollback_transaction()

    def truncate_and_checkpoint(self, trunc_start, trunc_stop, ts):
        c_start = self.session.open_cursor(self.uri)
        c_start.set_key(trunc_start)
        c_stop = self.session.open_cursor(self.uri)
        c_stop.set_key(trunc_stop)
        self.session.begin_transaction()
        self.session.truncate(None, c_start, c_stop, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        c_start.close()
        c_stop.close()
        self.leader_checkpoint(ts)

    def test_fast_truncate_disables_internal_delta(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        # Phase 1: insert all rows, checkpoint to establish the base image, then
        # evict all leaf pages to WT_REF_DISK (required for fast delete eligibility).
        self.setup_leader(',' + self.page_cfg)

        # Phase 1.5: before reopen, internal pages lack a valid disagg page_id; delta must be
        # rejected. Update one row and checkpoint to trigger reconciliation.
        rej_before        = self.get_stat(self.conn, stat.dsrc.rec_page_delta_rejected_invalid_page_id, self.uri)
        delta_pre_reopen  = self.get_stat(self.conn, stat.dsrc.rec_page_delta_internal, self.uri)
        cur = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cur[1] = self.value + 'y'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(12))
        cur.close()
        self.leader_checkpoint(12)
        rej_after = self.get_stat(self.conn, stat.dsrc.rec_page_delta_rejected_invalid_page_id, self.uri)
        self.assertGreater(rej_after, rej_before,
            "expected delta rejection due to invalid page_id before reopen")
        self.assertEqual(self.get_stat(self.conn, stat.dsrc.rec_page_delta_internal, self.uri),
            delta_pre_reopen,
            "no internal page delta should be written before reopen (page_id not yet assigned)")

        # Reopen so internal pages load from disk with valid disagg page_ids for delta writes.
        self.reopen_disagg_conn(self.conn_config())

        delta_int_before = self.get_stat(self.conn, stat.dsrc.rec_page_delta_internal, self.uri)
        rd_fast_before   = self.get_stat(self.conn, stat.conn.rec_page_delete_fast)
        read_del_before  = self.get_stat(self.conn, stat.conn.cache_read_deleted)

        # Phase 2: update rows outside the truncation range -- internal page delta must fire.
        cur = self.session.open_cursor(self.uri)
        for i in range(self.trunc_stop + 10, self.trunc_stop + 20):
            self.session.begin_transaction()
            cur[i] = self.value + 'x'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))
        cur.close()
        self.leader_checkpoint(15)

        delta_int_after_update = self.get_stat(self.conn, stat.dsrc.rec_page_delta_internal, self.uri)
        self.assertGreater(delta_int_after_update, delta_int_before,
            "internal page delta not written for normal update -- "
            "WT_INTERNAL_PAGE_DELTA may be disabled")

        # Phase 3: fast truncate + checkpoint -- internal page delta must be suppressed.
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)

        verify_cur = self.session.open_cursor(self.uri)
        verify_cur.set_key(self.trunc_start)
        self.assertEqual(verify_cur.search(), wiredtiger.WT_NOTFOUND)
        verify_cur.set_key(self.trunc_stop)
        self.assertEqual(verify_cur.search(), wiredtiger.WT_NOTFOUND)
        verify_cur.close()

        rd_fast_after   = self.get_stat(self.conn, stat.conn.rec_page_delete_fast)
        read_del_after  = self.get_stat(self.conn, stat.conn.cache_read_deleted)
        delta_int_after = self.get_stat(self.conn, stat.dsrc.rec_page_delta_internal, self.uri)

        # (1) Fast delete actually fired.
        self.assertGreater(rd_fast_after, rd_fast_before,
            "fast truncate did not trigger -- check page eligibility (evict, ts, range coverage)")

        # (2) No WT_REF_DELETED instantiation occurred.
        self.assertEqual(read_del_after, read_del_before,
            "WT_REF_DELETED was instantiated -- test setup has an unexpected reader")

        # (3) Internal page delta suppressed during the fast-truncate checkpoint.
        self.assertEqual(delta_int_after, delta_int_after_update,
            "regression: internal page delta was written despite a WT_REF_DELETED child "
            "(build_delta=false guard in rec_child.c may have been removed)")

if __name__ == '__main__':
    wttest.run()
