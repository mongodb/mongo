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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# Evict a clean disaggregated page into Palite's victim cache, then read it twice.
@disagg_test_class
class test_layered_victim_cache01(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    disagg_config = 'victim_cache_max_entries=10000'

    create_session_config = 'key_format=S,value_format=S'
    table_name = test_name
    nitems = 1000

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, [
        ('shared', dict(prefix='table:',
            table_config='block_manager=disagg,log=(enabled=false),leaf_page_max=4KB')),
    ])

    def evict(self, uri, key):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        cursor.reset()
        self.session.rollback_transaction()
        cursor.close()

    def test_victim_cache_evict_and_read(self):
        page_log = self.conn.get_page_log(self.vars.page_log)

        uri = self.prefix + self.table_name
        self.session.create(uri, self.create_session_config + ',' + self.table_config)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for i in range(self.nitems):
            cursor[f'k{i:06d}'] = 'v' * 64
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)
        page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, last_lsn)

        key = f'k{self.nitems // 2:06d}'
        puts_before = self.get_stat(stat.conn.block_cache_puts)
        self.evict(uri, key)
        self.evict(uri, key)
        self.assertGreater(self.get_stat(stat.conn.block_cache_puts), puts_before)

        cursor = self.session.open_cursor(uri, None, None)
        self.assertEqual(cursor[key], 'v' * 64)
        cursor.reset()
        self.assertEqual(cursor[key], 'v' * 64)
        cursor.close()

        page_log.terminate(self.session)
