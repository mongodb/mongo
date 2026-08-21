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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Verify the checkpoint cleanup thread lifecycle under disaggregated storage.
# Progress is checked via the checkpoint_cleanup_thread_start and
# checkpoint_cleanup_thread_stop connection statistics.
class checkpoint_cleanup_role_base(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),'
    initial_role = None

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="{self.initial_role}"),'

    def get_conn_stat(self, stat_key):
        c = self.session.open_cursor('statistics:')
        val = c[stat_key][2]
        c.close()
        return val

    def start_count(self):
        return self.get_conn_stat(wiredtiger.stat.conn.checkpoint_cleanup_thread_start)

    def stop_count(self):
        return self.get_conn_stat(wiredtiger.stat.conn.checkpoint_cleanup_thread_stop)


# Verify the cleanup thread lifecycle when the initial role is follower: opening leaves the
# thread stopped, step-up starts it, step-down stops it, and same-role reconfigure is a no-op.
@disagg_test_class
class test_layered_checkpoint_cleanup01(checkpoint_cleanup_role_base):
    initial_role = 'follower'

    def test_follower_open_leaves_thread_stopped(self):
        # Checkpoint cleanup is leader-only work; a follower open must not launch the thread.
        self.assertEqual(self.start_count(), 0)
        self.assertEqual(self.stop_count(), 0)

        # Same-role reconfigure is a no-op.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.assertEqual(self.start_count(), 0)
        self.assertEqual(self.stop_count(), 0)

    def test_role_transitions(self):
        self.assertEqual(self.start_count(), 0)
        self.assertEqual(self.stop_count(), 0)

        # First step-up starts the thread.
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.start_count(), 1)
        self.assertEqual(self.stop_count(), 0)

        # Same-role reconfigure is a no-op.
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(self.start_count(), 1)
        self.assertEqual(self.stop_count(), 0)

        # Cycle step-down / step-up: each transition moves exactly one counter by one.
        for i in range(1, 6):
            self.conn.reconfigure('disaggregated=(role="follower")')
            self.assertEqual(self.start_count(), i)
            self.assertEqual(self.stop_count(), i)

            # Stepping down again is a no-op.
            self.conn.reconfigure('disaggregated=(role="follower")')
            self.assertEqual(self.start_count(), i)
            self.assertEqual(self.stop_count(), i)

            self.conn.reconfigure('disaggregated=(role="leader")')
            self.assertEqual(self.start_count(), i + 1)
            self.assertEqual(self.stop_count(), i)


# Verify the cleanup thread launches once at open when the initial role is leader.
@disagg_test_class
class test_layered_checkpoint_cleanup01_leader_open(checkpoint_cleanup_role_base):
    initial_role = 'leader'

    def test_leader_open_starts_thread_once(self):
        self.assertEqual(self.start_count(), 1)
        self.assertEqual(self.stop_count(), 0)

        # Step down: exactly one stop.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.assertEqual(self.start_count(), 1)
        self.assertEqual(self.stop_count(), 1)
