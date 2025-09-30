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

import os, sys, time, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class

# test_layered07.py
#    Start a second WT that becomes leader and checke that content appears in the first.
@disagg_test_class
class test_layered07(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500

    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm,lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    uri = "layered:test_layered07"

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')
        self.pr(f"{extlist=}")

    # Custom test case setup
    def early_setup(self):
        os.mkdir('follower')
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    # Test inserting records into a follower that turned into a leader
    def test_layered07(self):
        if sys.platform.startswith('darwin'):
            return

        #
        # Part 1: Create a layered table and insert some data to the leader.
        #
        self.pr("create layered tree")
        self.session.create(self.uri, self.create_session_config)

        self.pr("create second WT")
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.create_session_config)

        self.pr('opening cursor')
        cursor = self.session.open_cursor(self.uri, None, None)

        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"
            cursor["Hi " + str(i)] = "There"
            cursor["OK " + str(i)] = "Go"
            if i % 250 == 0:
                time.sleep(1)

        # Ensure that all data makes it to the follower.
        cursor.close()
        time.sleep(1)
        self.session.checkpoint()
        time.sleep(1)
        self.disagg_advance_checkpoint(conn_follow)

        #
        # Part 2: The big switcheroo
        #
        self.pr('switch the leader and the follower')
        self.disagg_switch_follower_and_leader(conn_follow, self.conn)
        time.sleep(2)

        #
        # Part 3: Insert content to old follower
        #
        cursor = session_follow.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["* Hello " + str(i)] = "World"
            cursor["* Hi " + str(i)] = "There"
            cursor["* OK " + str(i)] = "Go"
            if i % 250 == 0:
                time.sleep(1)

        cursor.close()
        time.sleep(1)
        session_follow.checkpoint()
        self.disagg_advance_checkpoint(self.conn, conn_follow)

        #
        # Part 4: Ensure that all data is in both leader and follower.
        #
        cursor = session_follow.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 6)
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 6)
        cursor.close()
