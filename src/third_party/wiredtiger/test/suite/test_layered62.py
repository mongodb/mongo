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

# test_layered62.py
#    Test stepping down concurrently with a checkpoint.
#
# If WiredTiger makes a role change while a checkpoint is running, it could cause a part of a
# checkpoint to complete with the old role and a part with the new role, which would lead to an
# inconsistent checkpoint. This test verifies that stepping up and stepping down are properly
# synchronized with the checkpoint process.
#
# The test attempts to perform a role change while a checkpoint is running by starting the
# checkpoint in a separate thread, waiting for it to start, and then performing the role change.
# It then verifies that the checkpoint completed correctly with the old role, given that the role
# change happened after the checkpoint started.
#
@disagg_test_class
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
class test_layered62(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S,type=layered'

    uri = "table:test_layered62"

    disagg_storages = gen_disagg_storages('test_layered62', disagg_only = True)
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

    # Test stepping up concurrently with a checkpoint.
    def test_layered62(self):
        self.conn.reconfigure('disaggregated=(role="leader")')

        self.session.create(self.uri, self.create_session_config)

        # Create tables with some data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['a'] = 'value1'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        # Reopen the connection as a follower.
        self.restart_without_local_files()

        #
        # Part 1: Step up.
        #

        # Add more data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['b'] = 'value2'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Step up.
        self.pr('Stepping up')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Check that the most recent checkpoint was not a disagg checkpoint, which we can quickly
        # determine by checking its timestamp.
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.pr(f'Checkpoint timestamp: {checkpoint_timestamp}')
        self.assertEqual(checkpoint_timestamp, 1)

        # Complete the checkpoint and check that the timestamp has been updated.
        self.conn.reconfigure('timing_stress_for_test=[]')
        self.session.checkpoint()
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.pr(f'Checkpoint timestamp: {checkpoint_timestamp}')
        self.assertEqual(checkpoint_timestamp, 2)

        #
        # Part 2: Step down while a checkpoint is running.
        #

        # Add even more data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['c'] = 'value3'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Start a checkpoint in a separate thread.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_slow]')
        def checkpoint_thread_fn(conn):
            session = conn.open_session('')
            self.pr('Checkpoint started')
            # This checkpoint will take at least 10 seconds due to timing_stress_for_test.
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

        # Step down concurrently with the checkpoint.
        self.pr('Stepping down')
        self.conn.reconfigure('disaggregated=(role="follower")')
        checkpoint_thread.join()

        # No need for a timing stress after this point.
        self.conn.reconfigure('timing_stress_for_test=[]')

        # The most recent checkpoint was started as a leader; stepping down should not affect it.
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext()
        self.pr(f'Checkpoint timestamp: {checkpoint_timestamp}')
        self.assertEqual(checkpoint_timestamp, 3)

        # Reopen the connection.
        self.restart_without_local_files()

        # Check that all the data is present.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor['a'], 'value1')
        self.assertEqual(cursor['b'], 'value2')
        self.assertEqual(cursor['c'], 'value3')
        cursor.close()
