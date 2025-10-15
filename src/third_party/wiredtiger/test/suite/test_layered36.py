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

# test_layered36.py
#    Test creating missing stable tables.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered36(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S'

    table_name_empty = "test_layered36a"
    table_name_filled = "test_layered36b"

    disagg_storages = gen_disagg_storages('test_layered36', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('layered-type', dict(prefix='table:', table_config='block_manager=disagg,type=layered')),
    ])

    num_restarts = 0

    # Restart the node without local files
    def restart_without_local_files(self):
        # Close the current connection
        self.close_conn()

        # Move the local files to another directory
        self.num_restarts += 1
        dir = f'SAVE.{self.num_restarts}'
        os.mkdir(dir)
        for f in os.listdir():
            if os.path.isdir(f):
                continue
            if f.startswith('WiredTiger') or f.startswith('test_'):
                os.rename(f, os.path.join(dir, f))

        # Also save the PALM database (to aid debugging)
        shutil.copytree('kv_home', os.path.join(dir, 'kv_home'))

        # Reopen the connection
        self.open_conn()

    # A simple test with a single node.
    def test_layered36(self):

        # Create an empty table.
        uri_empty = self.prefix + self.table_name_empty
        config = self.create_session_config + ',' + self.table_config
        self.session.create(uri_empty, config)

        # Create a table with some data.
        uri_filled = self.prefix + self.table_name_filled
        config = self.create_session_config + ',' + self.table_config
        self.session.create(uri_filled, config)

        self.session.begin_transaction() # Draining requires timestamps
        cursor = self.session.open_cursor(uri_filled, None, None)
        cursor['a'] = 'b'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Step up and checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.checkpoint()

        # Restart without local files to check that the tables are created and have correct data.
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.restart_without_local_files()
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')

        # Check the tables
        cursor = self.session.open_cursor(uri_empty, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        cursor.close()
        self.assertEqual(item_count, 0)

        cursor = self.session.open_cursor(uri_filled, None, None)
        # FIXME-SLS-1824: This test triggers a KeyError,
        #  i.e. search for cursor['a'] returns WT_NOTFOUND?
        if False:
            self.assertEqual(cursor['a'], 'b')
        cursor.close()
