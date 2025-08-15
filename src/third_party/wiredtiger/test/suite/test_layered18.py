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

# test_layered18.py
#    Create long delta chains.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered18(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500
    key_to_update = 0
    num_updates = 10

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered18"

    disagg_storages = gen_disagg_storages('test_layered18', disagg_only = True)
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
        os.mkdir('follower')
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    # Test long delta chains
    def test_layered18(self):

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
            if i % 250 == 0:
                time.sleep(1)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp1)}')

        time.sleep(1)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp1)}')
        self.session.checkpoint()
        time.sleep(1)

        # Create several updates with small changes
        value_prefix2 = 'bbb'
        for n in range(1, self.num_updates + 1):
            self.session.begin_transaction()
            cursor = self.session.open_cursor(self.uri, None, None)
            cursor[str(self.key_to_update)] = value_prefix2 + str(self.key_to_update) + '-' + str(n)
            cursor.close()
            timestamp_n = timestamp1 + n
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp_n)}')
            time.sleep(1)
            self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp_n)}')
            self.session.checkpoint()

        # Create the follower
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')

        # Check the table in the follower: The call to GET will validate the delta chain in PALM.
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i == self.key_to_update:
                self.assertEqual(cursor[str(i)], value_prefix2 + str(i) + '-' + str(self.num_updates))
            else:
                self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()

        session_follow.close()
        conn_follow.close()
