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
from wiredtiger import stat

import time

# test_sweep07.py
# Regression test for WT-15647
class test_sweep07(wttest.WiredTigerTestCase):
    conn_config = 'file_manager=(close_scan_interval=1,close_idle_time=1,close_handle_minimum=1)'
    table_cfg = 'key_format=S,value_format=S'

    def test_sweep_with_cursor(self):
        uri = 'table:test_sweep07'

        # Create a table
        self.session.create(uri, self.table_cfg)

        # Create a cursor
        cursor = self.session.open_cursor(uri, None, None)
        cursor['a'] = 'b'
        cursor.close()

        # FIXME-WT-15646: We still check for LAS existence in __hs_cleanup_las() and according to
        # the current logic it creates an empty dhandle for it. Wait for enough time to sweep it
        # before starting the test.
        time.sleep(2)

        # Check the sweeping statistic
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        remove1 = stat_cursor[stat.conn.dh_sweep_remove][2]
        stat_cursor.close()

        # Checkpoint the table
        self.session.checkpoint()

        # Open another cursor and then close it
        session2 = self.conn.open_session('')
        cursor2 = session2.open_cursor(uri, None, None)
        cursor2.next()
        cursor2.close()
        session2.close()

        # Release the last reference by closing the session
        self.session.close()

        # Sleep to make the sweeping happen
        time.sleep(5)

        # Reopen the session and compare the statistic
        self.session = self.conn.open_session('')
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        remove2 = stat_cursor[stat.conn.dh_sweep_remove][2]
        stat_cursor.close()

        self.assertLess(remove1, remove2)

        self.conn.close()
