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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered90.py
#     Test that a follower correctly picks up multiple checkpoints for the same table.
#     Includes a cursor_copy variant to catch use-after-free bugs in accessing metadata cursor values.

@disagg_test_class
class test_layered90(wttest.WiredTigerTestCase):
    nitems = 100

    conn_base_config = 'statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    # cursor_copy variant: enables debug_mode=(cursor_copy=true) on the follower to
    # exercise the fixed code path under ASAN.
    cursor_copy_values = [
        ('no_cursor_copy', dict(cursor_copy=False)),
        ('cursor_copy',    dict(cursor_copy=True)),
    ]

    disagg_storages = gen_disagg_storages('test_layered90', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, cursor_copy_values)

    uri = 'layered:test_layered90'

    def follower_conn_config(self):
        cfg = self.extensionsConfig() + ',create,' + self.conn_base_config
        cfg += 'disaggregated=(role="follower")'
        if self.cursor_copy:
            cfg += ',debug_mode=(cursor_copy=true)'
        return cfg

    def insert_data(self, session, value_prefix):
        cursor = session.open_cursor(self.uri)
        for i in range(self.nitems):
            cursor[str(i)] = value_prefix + str(i)
        cursor.close()

    def check_data(self, session, value_prefix):
        cursor = session.open_cursor(self.uri)
        for i in range(self.nitems):
            self.assertEqual(cursor[str(i)], value_prefix + str(i))
        cursor.close()

    def test_follower_picks_up_updated_checkpoint(self):
        # Create the table on the leader and write initial data.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.insert_data(self.session, 'v1-')
        self.session.checkpoint()

        # Open the follower.
        conn_follow = self.wiredtiger_open('follower', self.follower_conn_config())
        session_follow = conn_follow.open_session('')

        # Checkpoint 1: follower picks up for the first time.
        self.disagg_advance_checkpoint(conn_follow)
        self.check_data(session_follow, 'v1-')

        # Write new data and take a second checkpoint on the leader.
        self.insert_data(self.session, 'v2-')
        self.session.checkpoint()

        # Checkpoint 2: follower picks up for the same table a second time.
        self.disagg_advance_checkpoint(conn_follow)
        self.check_data(session_follow, 'v2-')

        # A third round to verify repeated pickups remain correct.
        self.insert_data(self.session, 'v3-')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        self.check_data(session_follow, 'v3-')

        session_follow.close()
        conn_follow.close()
