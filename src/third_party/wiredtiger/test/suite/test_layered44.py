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

import os, time, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class
from wiredtiger import stat

# test_layered44.py
#    Ensure that we do not read any freed pages.
@disagg_test_class
class test_layered44(wttest.WiredTigerTestCase, DisaggConfigMixin):

    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader"),' + \
                  'verbose=[read:0,block:1],'

    nitems = 10_000

    table_name = 'test_layered44'
    uri = "layered:" + table_name
    stable_uri = "file:" + table_name + ".wt_stable"

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', 'palm')

    # Custom test case setup
    def early_setup(self):
        os.mkdir('follower')
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    # Test records into a layered tree and restarting
    def test_layered44(self):
        session_config = 'key_format=S,value_format=S'

        # Ignore all verbose output messages, as we're using them in this test.
        self.ignoreStdoutPattern("WT_VERB")

        #
        # Part 1: Create a layered table and a few checkpoints, and check the logged frees.
        #

        self.session.create(self.uri, session_config)

        self.assertEqual(self.get_stat(stat.conn.disagg_block_page_discard), 0)

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["Key " + str(i)] = str(i)
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 2 == 0:
                cursor["Key " + str(i)] = str(i) + "_even"
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 100 == 0:
                cursor["Key " + str(i)] = str(i) + "_hundred"
        cursor.close()

        self.session.checkpoint()

        # Get the stdout.txt file from the current working directory.
        stdout_path = os.path.join(os.getcwd(), 'stdout.txt')
        freed_pages = set()
        with open(stdout_path, 'r') as file:
            for line in file:
                if "WT_VERB_BLOCK" not in line or "block free" not in line \
                    or self.stable_uri not in line:
                    continue
                page_number = int(line.split('page_id')[1].split(',')[0].strip())
                self.assertNotIn(page_number, freed_pages, f"Page {page_number} freed more than once")
                freed_pages.add(page_number)
        self.assertGreater(len(freed_pages), 0)

        self.assertGreater(self.get_stat(stat.conn.disagg_block_page_discard), 0)

        #
        # Part 2: Open a follower, read the data, and check that we do not read any freed pages.
        #

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                           'verbose=[read:1],' +
                                           self.conn_base_config + "disaggregated=(role=\"follower\")")
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')

        cursor_follow = session_follow.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor_follow.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems)
        cursor_follow.close()

        session_follow.close()
        conn_follow.close()

        num_pages_read = 0
        with open(stdout_path, 'r') as file:
            for line in file:
                if "WT_VERB_READ" not in line or self.stable_uri not in line:
                    continue
                num_pages_read += 1
                page_number = int(line.split('page_id')[1].split(',')[0].strip())
                self.assertNotIn(page_number, freed_pages, f"Unexpected page read: {page_number}")
        self.assertGreater(num_pages_read, 0)
