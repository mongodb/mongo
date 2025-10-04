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

import os, os.path, shutil, time, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_layered43.py
#    Test disaggregated storage with block cache.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered43(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500
    key_to_update = 0
    num_updates = 10

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),' \
                     + 'block_cache=(enabled=true,type="dram",size=256MB),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered43"

    disagg_storages = gen_disagg_storages('test_layered43', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered', dict(prefix='layered:')),
        ('shared', dict(prefix='table:')),
    ])

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Custom test case setup
    def early_setup(self):
        os.mkdir('kv_home')

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    # Test long delta chains
    def test_layered43(self):

        # Create table
        self.uri = self.prefix + self.table_name
        table_config = self.create_session_config
        if not self.uri.startswith('layered'):
            table_config += ',block_manager=disagg,log=(enabled=false)'
        self.session.create(self.uri, table_config)

        # Put data to the table
        value_prefix1 = 'aaa'
        timestamp1 = 100
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor[str(i)] = value_prefix1 + str(i)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp1)}')

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp1)}')
        self.session.checkpoint()

        # Track block cache stats before evicting
        prev_block_cache_blocks_removed = self.get_stat(stat.conn.block_cache_blocks_removed)

        # Create several updates with small changes
        value_prefix2 = 'bbb'
        for n in range(1, self.num_updates + 1):
            self.session.begin_transaction()
            cursor = self.session.open_cursor(self.uri, None, None)
            last_value = value_prefix2 + str(self.key_to_update) + '-' + str(n)
            cursor[str(self.key_to_update)] = last_value
            cursor.close()
            timestamp_n = timestamp1 + n
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp_n)}')
            self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp_n)}')
            self.session.checkpoint()

        # Evict a page and read it back, which should put it in to the block cache
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.assertEqual(cursor[str(self.key_to_update)], last_value)
        cursor.reset()
        cursor.close()
        self.session.rollback_transaction()
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor[str(self.key_to_update)], last_value)
        cursor.close()
        self.session.rollback_transaction()

        # Evict the page again
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.assertEqual(cursor[str(self.key_to_update)], last_value)
        cursor.reset()
        cursor.close()
        self.session.rollback_transaction()

        self.assertGreater(self.get_stat(stat.conn.block_cache_blocks_removed), prev_block_cache_blocks_removed)

        # Remember the relevant statistics
        stat_cursor = self.session.open_cursor('statistics:')
        prev_cache_read = stat_cursor[wiredtiger.stat.conn.cache_read][2]
        prev_cache_pages_requested = stat_cursor[wiredtiger.stat.conn.cache_pages_requested][2]
        stat_cursor.close()

        # Read it again, which should be satisfied from the block cache
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor[str(self.key_to_update)], last_value)
        cursor.close()
        self.session.rollback_transaction()

        # Check the relevant statistics
        stat_cursor = self.session.open_cursor('statistics:')
        self.assertEqual(stat_cursor[wiredtiger.stat.conn.cache_read][2], prev_cache_read)
        self.assertGreater(stat_cursor[wiredtiger.stat.conn.cache_pages_requested][2],
                           prev_cache_pages_requested)
        stat_cursor.close()
