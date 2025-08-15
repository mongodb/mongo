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

# test_layered26.py
#    Make sure a secondary picking up a checkpoint adds in the stable
#    component of the table.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered26(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 5000

    conn_base_config = 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    session_create_config = 'key_format=S,value_format=S,'

    disagg_storages = gen_disagg_storages('test_layered26', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('layered-type', dict(prefix='table:', table_config='block_manager=disagg,type=layered,')),
    ])

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Custom test case setup
    def early_setup(self):
        os.mkdir('follower')
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def test_layered26(self):
        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=1')

        self.uri = self.prefix + 'test_layered26'

        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Create a secondary
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_config)
        session_follow = conn_follow.open_session('')

        # Create tables on both primary and secondary
        self.session.create(self.uri, self.session_create_config + self.table_config)
        session_follow.create(self.uri, self.session_create_config + self.table_config)

        # Put data into the primary
        value_prefix1 = 'aaa'
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nitems):
            cursor[str(i)] = value_prefix1 + str(i)
        cursor.close()

        # Create a checkpoint
        self.session.checkpoint()

        # Primary should see the data, secondary should not.
        cursor = self.session.open_cursor(self.uri)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        cursor.close()
        self.assertEqual(item_count, self.nitems)

        cursor_follow = session_follow.open_cursor(self.uri)
        item_count = 0
        while cursor_follow.next() == 0:
            item_count += 1
        cursor_follow.close()
        self.assertEqual(item_count, 0)

        # Notify the secondary of the new checkpoint
        self.disagg_advance_checkpoint(conn_follow)

        # Now, the data should be visible on the secondary.
        cursor_follow = session_follow.open_cursor(self.uri)
        item_count = 0
        while cursor_follow.next() == 0:
            item_count += 1
        cursor_follow.close()
        self.assertEqual(item_count, self.nitems)

        # TODO: once we start really thinking about failover, extend this test
        # to step up the secondary and make sure it can do "normal" things like
        # inserts and checkpoints:
        #
        # conn_follow.reconfigure('disaggregated=(role="leader")')
        #
        # It's broken right now because the table-draining code needs to do some
        # stuff like using internal sessions to open cursors, and that's being done
        # in SLS-1226.
