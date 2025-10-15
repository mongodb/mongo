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

# test_layered25.py
#    Start without local files and test historical reads.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered25(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered25"

    disagg_storages = gen_disagg_storages('test_layered25', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('layered-type', dict(prefix='table:', table_config='block_manager=disagg,type=layered')),
        ('shared', dict(prefix='table:', table_config='block_manager=disagg,log=(enabled=false)')),
    ])

    num_restarts = 0

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Custom test case setup
    def early_setup(self):
        os.mkdir('kv_home')

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

    # Start without local files and test historical reads.
    def test_layered25(self):
        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Create table
        self.uri = self.prefix + self.table_name
        config = self.create_session_config + ',' + self.table_config
        self.session.create(self.uri, config)

        # Put data to the table
        value_prefix1 = 'aaa'
        timestamp1 = 100
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor[str(i)] = value_prefix1 + str(i)
            if i % 250 == 0:
                time.sleep(1)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp1)}')

        time.sleep(1)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp1)}')
        self.session.checkpoint()
        time.sleep(1)

        # Update data in the table with a different timestamp
        value_prefix2 = 'bbb'
        timestamp2 = 200
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor[str(i)] = value_prefix2 + str(i)
            if i % 250 == 0:
                time.sleep(1)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp2)}')

        time.sleep(1)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp2)}')
        self.session.checkpoint()
        time.sleep(1)

        # Check the data with the first timestamp
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp1)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()
        self.session.rollback_transaction()

        # Check the data with the second timestamp
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp2)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix2 + str(i))
        cursor.close()
        self.session.rollback_transaction()

        #
        # Part 1: Just reopen the connection, keeping local files
        #

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.reopen_conn()
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        self.conn.reconfigure(f'disaggregated=(role="leader")')

        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp2)}')

        # Check the data with the second timestamp
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp2)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix2 + str(i))
        cursor.close()
        self.session.rollback_transaction()

        # Check the data with the first timestamp (uses the history store)
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp1)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()
        self.session.rollback_transaction()

        #
        # Part 2: Restart without local files
        #

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.restart_without_local_files()
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        self.conn.reconfigure(f'disaggregated=(role="leader")')

        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp2)}')

        # Check the data with the second timestamp
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp2)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix2 + str(i))
        cursor.close()
        self.session.rollback_transaction()

        # Check the data with the first timestamp (uses the history store)
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(timestamp1)}')
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()
        self.session.rollback_transaction()
