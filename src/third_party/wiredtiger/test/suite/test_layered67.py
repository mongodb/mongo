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
from helper import WiredTigerStat
from wiredtiger import stat


# test_layered67.py
# Test that we write the page for update restore even when deltas are disabled.

@disagg_test_class
class test_layered67(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),'
    disagg_storages = gen_disagg_storages('test_layered67', disagg_only = True)

    nrows = 10
    uri='file:test_layered67'

    scenarios = make_scenarios(disagg_storages)

    def session_create_config(self):
        cfg = 'key_format=i,value_format=S,block_manager=disagg'
        return cfg

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),page_delta=(internal_page_delta=false,leaf_page_delta=false)'

    def test_uncommit_eviction(self):
        self.session.create(self.uri, self.session_create_config())
        with WiredTigerStat(self.session, self.uri) as stat_cursor:
            cache_put_before = stat_cursor[stat.dsrc.cache_write][2]
        self.session.begin_transaction()

        # Populate the table with nrows.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = "value1"
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))

        self.session.begin_transaction()
        cursor[1] = "value2"

        # Evict the data.
        session = self.conn.open_session("debug=(release_evict_page)")
        evict_cursor = session.open_cursor(self.uri, None, None)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # Monitor the status after eviction.
        with WiredTigerStat(self.session, self.uri) as stat_cursor:
            cache_put_after = stat_cursor[stat.dsrc.cache_write][2]
        self.assertGreater(cache_put_after, cache_put_before)
