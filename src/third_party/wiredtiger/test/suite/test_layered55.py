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
from eviction_util import eviction_util
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios


# Test we don't review obsolete time window for readonly btree in follower.
@disagg_test_class
class test_layered55(eviction_util, wttest.WiredTigerTestCase):
    conn_base_config = 'cache_size=10MB,'

    disagg_storages = gen_disagg_storages('test_layered55', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)
    uri='layered:test_layered55'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    def conn_config_follower(self):
        return self.conn_base_config + 'disaggregated=(role="follower"),'

    def read(self, nrows):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nrows):
            cursor.set_key(i)
            cursor.search()
            if i % 5 == 0:
                cursor.reset()
        cursor.close()

    def test_obsolete_time_window(self):
        create_params = 'key_format=i,value_format=S,block_manager=disagg'
        nrows = 10000
        value = 'k' * 1024

        self.session.create(self.uri, create_params)

        # Write some data on leader mode.
        self.populate(self.uri, 0, nrows, value)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows))
        self.session.checkpoint()
        # Reopen as follower.
        self.reopen_disagg_conn(self.conn_config_follower())
        # Read data into cache.
        self.read(nrows)

        # Set oldest timestamp to an older value.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(nrows // 2))
        # Read data again which triggers eviction.
        self.read(nrows)
        # We should not review obsolete time window as btree is readonly.
        btree_stat = self.get_stat(stat.dsrc.cache_eviction_dirty_obsolete_tw, self.uri)
        conn_stat = self.get_stat(stat.conn.cache_eviction_dirty_obsolete_tw)
        self.assertEqual(btree_stat, 0)
        self.assertEqual(conn_stat, 0)
