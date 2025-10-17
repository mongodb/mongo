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

import os, os.path, shutil, threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered60.py
#    Test creating empty tables while a checkpoint is running.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered60(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,' \
                     + 'timing_stress_for_test=[checkpoint_slow],'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S,type=layered'

    uri = "table:test_layered60"

    disagg_storages = gen_disagg_storages('test_layered60', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    num_restarts = 0

    # Restart the node without local files
    def restart_without_local_files(self):
        # Close the current connection
        self.close_conn()

        # Move the local files to another directory
        self.num_restarts += 1
        dir = f'SAVE.{self.num_restarts}'
        os.mkdir(dir)
        for f in os.listdir():
            if os.path.isdir(f):
                continue
            if f.startswith('WiredTiger') or f.startswith('test_'):
                os.rename(f, os.path.join(dir, f))

        # Also save the PALI database (to aid debugging)
        shutil.copytree('kv_home', os.path.join(dir, 'kv_home'))

        # Reopen the connection
        self.open_conn()

    # Wait for a checkpoint to start running
    def wait_for_checkpoint_start(self):
        while True:
            stat_cursor = self.session.open_cursor('statistics:')
            state = stat_cursor[wiredtiger.stat.conn.checkpoint_state][2]
            stat_cursor.close()
            if state != 0:
                break
            time.sleep(0.1)

    # Test creating an empty table while a checkpoint is running.
    def test_layered60(self):
        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=1')

        # Create a table with some data
        self.session.create(self.uri + 'x', self.create_session_config)
        cursor = self.session.open_cursor(self.uri + 'x', None, None)
        cursor['a'] = 'b'
        cursor.close()

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
        time.sleep(0.1)

        # Create an empty table and wait for the checkpoint to complete
        self.pr('Creating empty table')
        self.session.create(self.uri, self.create_session_config)
        checkpoint_thread.join()

        # No need for a timing stress after this point
        self.conn.reconfigure('timing_stress_for_test=[]')

        # Create a checkpoint
        self.session.checkpoint()

        #
        # Part 1: Check the new table in the follower
        #

        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_config)
        self.disagg_advance_checkpoint(conn_follow)
        session_follow = conn_follow.open_session('')

        # Check that the table exists in the follower
        cursor = session_follow.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        cursor.close()
        self.assertEqual(item_count, 0)

        # Clean up
        session_follow.close()
        conn_follow.close()

        #
        # Part 2: Check the new table after restart
        #

        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.restart_without_local_files()

        # Pick up the latest checkpoint and then step up as the leader. Do this in two steps, to
        # mimic the behavior of the server.
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        self.conn.reconfigure(f'disaggregated=(role="leader")')

        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=1')

        # Check that the table exists and is empty
        cursor = self.session.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        cursor.close()
        self.assertEqual(item_count, 0)
