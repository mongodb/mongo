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

# test_layered17.py
#    Check timestamps.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered17(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered17"

    disagg_storages = gen_disagg_storages('test_layered17', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('layered-type', dict(prefix='table:', table_config='block_manager=disagg,type=layered')),
        ('shared', dict(prefix='table:', table_config='block_manager=disagg,log=(enabled=false)')),
    ])

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')

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

    # Test timestamps
    def test_layered17(self):

        # Create table
        self.uri = self.prefix + self.table_name
        config = self.create_session_config + ',' + self.table_config
        self.session.create(self.uri, config)

        #
        # Phase 1: Add data at timestamp 100, stable timestamp 100
        #

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

        # Check the timestamps
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(timestamp1, checkpoint_timestamp)

        # Create the follower
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')

        # Check the table in the follower
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()

        #
        # Phase 2: Add data at timestamp 200, stable timestamp 200
        #

        # Put more data to the table
        value_prefix2 = 'bbb'
        timestamp2 = 200
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 50 == 0:
                cursor[str(i)] = value_prefix2 + str(i)
            if i % 250 == 0:
                time.sleep(1)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp2)}')

        time.sleep(1)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp2)}')
        self.session.checkpoint()
        time.sleep(1)

        # Check the timestamps
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(timestamp2, checkpoint_timestamp)

        # Pick up the new checkpoint
        self.disagg_advance_checkpoint(conn_follow)

        # Check the table in the follower
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 50 == 0:
                self.assertEqual(cursor[str(i)], value_prefix2 + str(i))
            else:
                self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()

        #
        # Phase 3: Add data at timestamp 300, stable timestamp 250
        #

        # Put more data to the table
        value_prefix3 = 'ccc'
        timestamp3 = 300
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 25 == 0:
                cursor[str(i)] = value_prefix3 + str(i)
            if i % 250 == 0:
                time.sleep(1)
        cursor.close()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp3)}')

        time.sleep(1)
        stable_timestamp3 = (timestamp2 + timestamp3) // 2
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(stable_timestamp3)}')
        self.session.checkpoint()
        time.sleep(1)

        # Check the timestamps
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.assertEqual(stable_timestamp3, checkpoint_timestamp)

        # Pick up the new checkpoint
        self.disagg_advance_checkpoint(conn_follow)

        # Check the table in the follower (should not see the latest changes)
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 50 == 0:
                self.assertEqual(cursor[str(i)], value_prefix2 + str(i))
            else:
                self.assertEqual(cursor[str(i)], value_prefix1 + str(i))
        cursor.close()

        session_follow.close()
        conn_follow.close()
