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

# test_layered80.py
# Test that the sweep server does not close ingest table dhandles and layered dhandles on a follower
# or during step-up. Closing the ingest dhandle discards all in-memory data for
# that table (WT-16974, WT-16703).
# Also tests that the sweep server does not close a layered dhandle that has pending follower
# truncate state (entries in its in-memory truncate list), as doing so would discard the truncate
# entries and corrupt visibility (WT-16798).

import time, wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

@disagg_test_class
class test_layered80(wttest.WiredTigerTestCase):
    # Use aggressive sweep settings so the server has every opportunity to
    # incorrectly close the ingest dhandle while we are running as follower.
    conn_config = 'statistics=(all),' \
                  'file_manager=(close_handle_minimum=0,close_idle_time=1,close_scan_interval=1),' \
                  'verbose=(sweep:3),' \
                  'disaggregated=(role="follower")'

    uri = 'layered:test_layered80'
    nrows = 1000

    disagg_storages = gen_disagg_storages('test_layered80', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def wait_for_sweep(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        baseline = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()
        for _ in range(120):
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            sweeps = stat_cursor[stat.conn.dh_sweeps][2]
            stat_cursor.close()
            # Sweep server only closes after the handle goes through multiple phases.
            if sweeps - baseline >= 3:
                return
            time.sleep(1)
        self.assertTrue(False, 'sweep server did not run within 120s')

    def test_layered_dhandle_not_swept_during_stepup(self):
        """
        Verify that the sweep server does not close the layered dhandle. During
        step-up, the ingest table is drained into the layered table. If the
        layered dhandle was closed by sweep beforehand, the drain operation
        cannot reference the existing layered data and leaves gaps (WT-16974,
        WT-16703).
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Write the first batch as follower with timestamps.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.nrows))

        # Pin the ingest table dhandle, so it doesn't get swept away on purpose.
        cursor = self.session.open_cursor("file:test_layered80.wt_ingest")

        # Wait for the sweep server to run several cycles. If it is not configured
        # to skip layered dhandles, it would mark and close them, causing gaps when
        # draining the ingest table at step-up.
        self.wait_for_sweep()

        # Step up to leader.
        self.conn.reconfigure('disaggregated=(role="leader")')
        cursor.close()

        # All rows from both batches must be present with no gaps.
        # Missing rows indicate the layered dhandle was incorrectly swept.
        cursor = self.session.open_cursor(self.uri)
        count = 0
        while (ret := cursor.next())  == 0:
            count += 1
        cursor.close()
        self.assertEqual(wiredtiger.WT_NOTFOUND, ret)
        self.assertEqual(count, self.nrows)

        self.ignoreStdoutPattern('WT_VERB_SWEEP')

    def test_layered_dhandle_not_swept_with_truncate_state(self):
        """
        Verify that the sweep server does not close the layered dhandle while it holds
        pending follower truncate state (entries in its in-memory truncate list).
        If the dhandle were closed during this window, the truncate entries are
        discarded.
        """
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled.")

        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Write data as follower with timestamps.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = 'value' + str(i)
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.nrows))

        # Begin a truncate transaction. On follower, this inserts an entry into the
        # layered dhandle's in-memory truncate list  state that must not be swept away.
        c_start = self.session.open_cursor(self.uri)
        c_start.set_key(100)
        c_stop = self.session.open_cursor(self.uri)
        c_stop.set_key(700)

        self.session.begin_transaction()
        self.session.truncate(None, c_start, c_stop, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.nrows + 1))

        # Wait for the sweep server to run several cycles. If the layered dhandle is
        # incorrectly swept while the truncate entry lives in its truncate list,
        # the truncated range will go missing.
        self.wait_for_sweep()

        c_start.close()
        c_stop.close()

        # Verify the truncate still exists: rows 100-700 should be gone.
        cursor = self.session.open_cursor(self.uri)
        count = 0
        while (ret := cursor.next()) == 0:
            count += 1
        cursor.close()
        self.assertEqual(wiredtiger.WT_NOTFOUND, ret)
        # rows 0-99 = 100 rows, rows 701-999 = 299 rows
        self.assertEqual(count, 100 + (self.nrows - 701))

        self.ignoreStdoutPattern('WT_VERB_SWEEP')
