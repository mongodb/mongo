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

# When an instantiated fast-truncate leaf has its deletion become globally visible, the
# parent reconciliation must rebuild a full base image rather than a delta, so that
# dropping the leaf's proxy cell and freeing its block happen together. This keeps the
# parent image free of any reference to the freed leaf block and verify passes.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered_fast_truncate21(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri         = f'layered:{test_name}'
    nrows       = 200
    value       = 'a' * 50
    trunc_start = 50
    trunc_stop  = 150

    # Small pages produce many leaves and multiple non-root internal pages (3-level tree);
    # only non-root internals are eligible for an internal page delta.
    page_cfg    = 'allocation_size=512,leaf_page_max=512,internal_page_max=512'

    conn_base_config = 'cache_size=50MB,statistics=(all),' \
                       'page_delta=(internal_page_delta=true,leaf_page_delta=false,delta_pct=100),'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
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

    def test_instantiated_truncate_leaf_resurrection(self):
        # Phase 1: populate, evict leaves to disk (fast delete eligibility), and reopen so
        # internal pages reload with valid page ids (delta-build eligible).
        self.setup_leader(',' + self.page_cfg)
        self.reopen_disagg_conn(self.conn_config())

        rd_fast_before = self.get_stat(self.conn, stat.conn.rec_page_delete_fast)
        read_del_before = self.get_stat(self.conn, stat.conn.cache_read_deleted)

        # Phase 2: fast-truncate and checkpoint with oldest still at 1, so the deletion is not
        # globally visible and the parent keeps a proxy cell referencing each truncated leaf.
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        self.assertGreater(self.get_stat(self.conn, stat.conn.rec_page_delete_fast),
            rd_fast_before, "fast truncate did not trigger -- check page eligibility")

        # Phase 3: read keys inside the truncated range below the truncate timestamp to
        # instantiate the fully-covered leaves (modify->instantiated set).
        cur = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        inst_keys = list(range(self.trunc_start + 1, self.trunc_start + 31)) + \
                    list(range(self.trunc_stop - 30, self.trunc_stop))
        for key in inst_keys:
            cur.set_key(key)
            self.assertEqual(cur.search(), 0)
            self.assertEqual(cur.get_value(), self.value)
        self.session.rollback_transaction()
        cur.close()
        self.assertGreater(self.get_stat(self.conn, stat.conn.cache_read_deleted),
            read_del_before, "truncated leaf was not instantiated")

        # Phase 4: advance oldest past the truncate so the deletion is globally visible.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30) +
                                ',stable_timestamp=' + self.timestamp_str(30))

        # Phase 5: dirty only the two boundary leaves so the parent reconciles in delta-build
        # mode (a larger change would fall back to a full base) while still holding the
        # instantiated truncated leaves.
        cur = self.session.open_cursor(self.uri)
        for i in (self.trunc_start - 1, self.trunc_stop + 1):
            self.session.begin_transaction()
            cur[i] = self.value + 'z'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(35))
        cur.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))
        self.session.checkpoint()

        # Phase 6: verify should pass
        self.session.verify(self.uri, None)

if __name__ == '__main__':
    wttest.run()
