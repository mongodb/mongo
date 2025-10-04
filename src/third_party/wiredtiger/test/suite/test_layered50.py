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

import wttest
from wiredtiger import stat
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Test we can evict on the follower without setting page materialization frontier

@disagg_test_class
class test_layered50(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered50', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = 'disaggregated=(page_log=palm),cache_size=10MB,,statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    nitems = 10

    session_follow = None
    conn_follow = None

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

    def test_evict_on_standby(self):
        uri = "layered:test_layered50"
        # Setup
        self.session.create(uri, 'key_format=S,value_format=S')
        self.create_follower()

        # Insert some data and checkpoint.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = str(i)
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(10)}")

        self.session.checkpoint()
        # Advance the checkpoint on the follower.
        self.disagg_advance_checkpoint(self.conn_follow)

        # Force the page to be evicted
        session_evict = self.conn_follow.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            evict_cursor.set_key(str(i))
            self.assertEqual(evict_cursor[str(i)], str(i))
            evict_cursor.reset()
        evict_cursor.close()

        # Verify we have done some evictions
        stat_cursor = self.session_follow.open_cursor('statistics:')
        self.assertGreater(stat_cursor[stat.conn.cache_eviction_clean][2], 0)
        stat_cursor.close()
