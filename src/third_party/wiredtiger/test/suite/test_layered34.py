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

# test_layered34.py
#    Test materialization frontier.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered34(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm,' \
                     + 'lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered34"

    disagg_storages = gen_disagg_storages('test_layered34', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        # Use shared tables directly to make testing easier
        ('shared', dict(prefix='table:', table_config='block_manager=disagg,log=(enabled=false)')),
    ])

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Custom test case setup
    def early_setup(self):
        os.mkdir('kv_home')

    # Test creating an empty table.
    def test_layered34(self):
        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=1')

        page_log = self.conn.get_page_log('palm')

        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Create table
        self.uri = self.prefix + self.table_name
        config = self.create_session_config + ',' + self.table_config
        self.session.create(self.uri, config)

        # Add some data and create a checkpoint
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['a'] = last_value = 'b'
        cursor.close()
        self.session.checkpoint()

        (ret, checkpoint1_last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        # Add more data and create another checkpoint
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['a'] = last_value = 'c'
        cursor.close()
        self.session.checkpoint()

        (ret, checkpoint2_last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        page_log.pl_set_last_materialized_lsn(self.session, checkpoint1_last_lsn)
        self.conn.reconfigure(f'disaggregated=(last_materialized_lsn={checkpoint1_last_lsn})')

        # Evict the page
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.assertEqual(cursor['a'], last_value)
        cursor.reset()
        cursor.close()
        self.session.rollback_transaction()

        # Check the data
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor['a'], last_value)
        cursor.close()
