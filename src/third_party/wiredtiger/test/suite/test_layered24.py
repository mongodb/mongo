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

import os
import wiredtiger
import wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class

# test_layered24.py
#    Ensure a secondary that drops a table does not fall back to reading
#    the stable table.
@disagg_test_class
class test_layered24(wttest.WiredTigerTestCase, DisaggConfigMixin):
    uri = "layered:test_layered24"

    conn_base_config = "disaggregated=(page_log=palm),"
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    nitems = 10000

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    def early_setup(self):
        os.mkdir("follower")
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def test_layered24(self):
        session_config = 'key_format=S,value_format=S'

        #
        # Part 1: Create a layered table and a follower that also has that data.
        #
        self.session.create(self.uri, session_config)
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, session_config)

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"
            cursor["Hi " + str(i)] = "There"
            cursor["OK " + str(i)] = "Go"
        cursor.close()

        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Sanity-check all data made it to the follower.
        cursor_follow = session_follow.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor_follow.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 3)
        cursor_follow.close()

        #
        # Part 2: drop the table on the secondary and check it has no data.
        #
        session_follow.drop(self.uri, 'force=true')

        with self.assertRaises(wiredtiger.WiredTigerError):
            session_follow.open_cursor(self.uri, None, None)

        #
        # Part 3: check we still have content on the leader, then drop and
        # perform the same check that all the content is gone.
        #
        cursor = self.session.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 3)
        cursor.close()

        # Avoid any shenanigans with cached cursors, etc
        self.reopen_conn()

        self.session.drop(self.uri)
        with self.assertRaises(wiredtiger.WiredTigerError):
            self.session.open_cursor(self.uri, None, None)
