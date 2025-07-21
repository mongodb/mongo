#!/usr/bin/env python
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

import time
from test_cc01 import test_cc_base
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc10.py
# Test that the obsolete cleanup thread can be configured to run at different intervals.
class test_cc10(test_cc_base):
    conn_config_common = 'statistics=(all),statistics_log=(json,wait=1,on_close=true),verbose=(checkpoint_cleanup:1)'

    waiting_time = [
        ('1sec_wait_0sec_file_wait', dict(conn_config=f'{conn_config_common},checkpoint_cleanup=[wait=1,file_wait_ms=0]')),
        ('1sec_wait_1sec_file_wait', dict(conn_config=f'{conn_config_common},checkpoint_cleanup=[wait=1,file_wait_ms=1000]')),
        ('2sec_wait_2sec_file_wait', dict(conn_config=f'{conn_config_common},checkpoint_cleanup=[wait=2,file_wait_ms=2000]')),
        ('3sec_wait_3sec_file_wait', dict(conn_config=f'{conn_config_common},checkpoint_cleanup=[wait=3,file_wait_ms=3000]')),
    ]

    scenarios = make_scenarios(waiting_time)

    # We enabled verbose log level DEBUG_1 in this test, ignore the output.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_CHECKPOINT_CLEANUP')

    def test_cc10(self):
        # Create a table.
        create_params = 'key_format=i,value_format=S'
        nrows = 1000
        uri = "table:cc10"

        # Create and populate a table.
        self.session.create(uri, create_params)

        old_value = "a"
        old_ts = 1
        self.populate(uri, 0, nrows, old_value, old_ts)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(old_ts)},stable_timestamp={self.timestamp_str(old_ts)}')

        # Update each record with a newer timestamp.
        new_value = "b"
        new_ts = 10
        self.populate(uri, 0, nrows, new_value, new_ts)

        # Make the updates stable and checkpoint to write everything in the DS and HS.
        # The most recent updates with the ts 10 should be in the DS while the ones with ts 1 should
        # be in the HS.
        self.conn.set_timestamp(f'stable_timestamp={new_ts}')
        self.session.checkpoint()

        # Make the updates in the HS obsolete.
        self.conn.set_timestamp(f'oldest_timestamp={new_ts}')

        # Wait for obsolete cleanup to occur, this should clean the history store.
        # We don't use the debug option to force cleanup as the thread should be running in the
        # background.
        time.sleep(5)
        self.wait_for_cc_to_run()

        c = self.session.open_cursor('statistics:')
        visited = c[stat.conn.checkpoint_cleanup_pages_visited][2]
        obsolete_evicted = c[stat.conn.checkpoint_cleanup_pages_evict][2]
        obsolete_on_disk = c[stat.conn.checkpoint_cleanup_pages_removed][2]
        c.close()

        # We should always visit pages for cleanup.
        self.assertGreater(visited, 0)

        # We should have performed some cleanup.
        self.assertGreater(obsolete_evicted + obsolete_on_disk, 0)
