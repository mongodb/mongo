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

import threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered71.py
#    Test dropping empty tables while a checkpoint is running.
@disagg_test_class
class test_layered71(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,' \
                     + 'file_manager=(close_scan_interval=1,close_idle_time=1,close_handle_minimum=0),' \
                     + 'timing_stress_for_test=[checkpoint_slow],'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S,type=layered'

    uri = "table:test_layered71"

    disagg_storages = gen_disagg_storages('test_layered71', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Wait for a checkpoint to start running
    def wait_for_checkpoint_start(self):
        while True:
            stat_cursor = self.session.open_cursor('statistics:')
            state = stat_cursor[wiredtiger.stat.conn.checkpoint_state][2]
            stat_cursor.close()
            if state != 0:
                break
            time.sleep(0.1)

    # Test dropping an empty table while a checkpoint is running.
    def test_layered71(self):
        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=1')

        # Create an empty table
        session2 = self.conn.open_session('')
        session2.create(self.uri, self.create_session_config)
        session2.close()

        # Wait until the sweep has closed any idle handles
        while True:
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            sweep_closes = stat_cursor[wiredtiger.stat.conn.dh_sweep_dead_close][2]
            stat_cursor.close()
            if sweep_closes > 0:
                self.pr(f"Dhandles closed by sweep: {sweep_closes}")
                break
            time.sleep(0.5)

        # Start a checkpoint in a separate thread
        def checkpoint_thread_fn(conn):
            session = conn.open_session('')
            self.pr('Checkpoint started')
            # This checkpoint will take at least 10 seconds due to timing_stress_for_test
            session.checkpoint()
            self.pr('Checkpoint complete')
            session.close()
        checkpoint_thread = threading.Thread(target=checkpoint_thread_fn, args=(self.conn,))
        checkpoint_thread.start()

        # Wait for the checkpoint to start, and then a tiny bit more just in case. There should be
        # enough time for us to do this, because the checkpoint will take at least 10 seconds due
        # to the timing stress.
        self.wait_for_checkpoint_start()
        time.sleep(0.5)

        # Drop the empty table and wait for the checkpoint to complete
        self.pr('Dropping empty table')
        self.session.drop(self.uri, 'checkpoint_wait=false')
        self.pr('Dropped the table')
        checkpoint_thread.join()

        # No need for a timing stress after this point
        self.conn.reconfigure('timing_stress_for_test=[]')

        # Check that the table still exists in the follower and is empty
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_config)
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')
        cursor = session_follow.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        cursor.close()
        self.assertEqual(item_count, 0)

        # Clean up
        session_follow.close()
        conn_follow.close()
