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

import re
import wiredtiger
import wttest
from metadata_helper import extract_id
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered86.py
# Make sure that a follower picks up and applies new file IDs.
@disagg_test_class
class test_layered86(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'
    conn_config_follower = 'disaggregated=(role="follower")'

    uri = "layered:test_layered86"

    disagg_storages = gen_disagg_storages('test_layered86', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_standby_uses_table_id_high_water_mark(self):
        # Make 100 tables, then checkpoint.
        for i in range(0, 100):
            self.session.create(f"layered:test_layered86_{i}", 'key_format=S,value_format=S')
        self.conn.set_timestamp('stable_timestamp=1') # Don't upset precise checkpoint
        self.session.checkpoint()

        # Record the highest file ID.
        md_cursor = self.session.open_cursor('metadata:', None, None)
        max_file_id = 0
        for key, value in md_cursor:
            if not key.startswith('file:') and not key == 'metadata:':
                continue

            curr_file_id = extract_id(value)
            if curr_file_id > max_file_id:
                max_file_id = curr_file_id

        # Drop those tables, checkpoint again.
        for i in range(0, 100):
            self.session.drop(f"layered:test_layered86_{i}")
        self.session.checkpoint()

        # Make a follower and feed it the latest checkpoint.
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')
        self.disagg_advance_checkpoint(self.conn_follow)

        # Kill the leader. Skip the closing checkpoint -- otherwise, when the follower connection
        # is closed, we'll discard unclean pages twice. These pages can have the same ID, which
        # makes PALite think it's seeing a double-free.
        self.session.close()
        self.conn.close('debug=(skip_checkpoint=true)')

        # Follower step-up to leader.
        self.conn_follow.reconfigure('disaggregated=(role="leader")')

        # Make a new table on the (new) leader. Checkpoint.
        self.conn_follow.set_timestamp('stable_timestamp=2') # Don't upset precise checkpoint
        self.session_follow.create(f"layered:test_layered86_101", 'key_format=S,value_format=S')
        self.session_follow.checkpoint()

        # Make sure the table ID is higher than what we saw from the old leader
        md_cursor = self.session_follow.open_cursor('metadata:', None, None)
        follower_max_file_id = 0
        for key, value in md_cursor:
            if not key.startswith('file:') and not key == 'metadata:':
                continue

            curr_file_id = extract_id(value)
            if curr_file_id > follower_max_file_id:
                follower_max_file_id = curr_file_id
        self.assertTrue(follower_max_file_id > max_file_id)
