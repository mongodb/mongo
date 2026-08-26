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

# test_layered_drop01.py
# Dropping a layered table must not read from the page log.
# If an existing table has been closed by the connection sweep, we want to make
# sure that a drop does not reopen the table while getting exclusive access to it.

import time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered_drop01(wttest.WiredTigerTestCase):
    nitems = 5000
    uri = 'layered:drop01'

    conn_base_config = 'statistics=(all),' + \
        'file_manager=(close_idle_time=1,close_scan_interval=1,close_handle_minimum=0),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def populate_and_checkpoint(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor['key ' + str(i)] = 'value ' + str(i)
        cursor.close()

        self.session.checkpoint()

    # Returns the number of block reads a drop performs
    def reads_during_drop(self):
        before = self.get_stat(stat.conn.disagg_block_get)
        self.session.drop(self.uri, None)
        after = self.get_stat(stat.conn.disagg_block_get)

        # Ensure that the table has really been dropped,
        # expect an error on reopen.
        with self.assertRaises(wiredtiger.WiredTigerError):
            self.session.open_cursor(self.uri, None, None)

        return after - before

    def test_drop_after_conn_reopen(self):
        self.populate_and_checkpoint()

        # Reopening the connection leaves the table's data handles closed.
        self.reopen_conn()

        self.assertEqual(self.reads_during_drop(), 0,
            'dropping a layered table with a closed data handle read from the page log')

    def test_drop_after_sweep(self):
        self.populate_and_checkpoint()

        # Wait for the sweep server to close idle data handles.  The populated
        # table has no cursors open, so it should be closed.
        baseline = self.get_stat(stat.conn.dh_conn_handle_count)
        deadline = time.time() + 30
        while True:
            self.session.reset()
            count = self.get_stat(stat.conn.dh_conn_handle_count)
            if self.get_stat(stat.conn.dh_sweep_expired_close) > 0 and count < baseline:
                break
            self.assertLess(time.time(), deadline,
                'timed out waiting for sweep to close idle data handles')
            time.sleep(0.5)

        self.assertEqual(self.reads_during_drop(), 0,
            'dropping a layered table with a swept data handle read from the page log')

    def test_drop_with_open_handle(self):
        # With the data handle still open we expect the drop to read nothing.
        self.populate_and_checkpoint()

        self.assertEqual(self.reads_during_drop(), 0,
            'dropping a layered table with an open data handle read from the page log')
