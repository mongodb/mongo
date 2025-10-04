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

import os, glob, time, wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtbackup import backup_base


# test_live_restore08.py
# Test bulk cursor usage with live restore.
class test_live_restore08(backup_base):
    nrows = 10000

    def get_stat(self, statistic):
        stat_cursor = self.session.open_cursor("statistics:")
        val = stat_cursor[statistic][2]
        stat_cursor.close()
        return val

    def wait_for_live_restore_complete(self):
        state = 0
        timeout = 120
        iteration_count = 0
        # Build in a 2 minute timeout. Once we see the complete state exit the loop.
        while (iteration_count < timeout):
            state = self.get_stat(stat.conn.live_restore_state)
            self.pr(f'Looping until finish, live restore state is: {state}, \
                      Current iteration: is {iteration_count}')
            if (state == wiredtiger.WT_LIVE_RESTORE_COMPLETE):
                break
            time.sleep(1)
            iteration_count += 1
        self.assertEqual(state, wiredtiger.WT_LIVE_RESTORE_COMPLETE)

    def populate_backup(self):
        ds1 = SimpleDataSet(self, 'file:standard', self.nrows,
        key_format='i', value_format='S')
        ds1.populate()

        self.session.create('file:bulk', f'key_format=i,value_format=S')

        self.session.checkpoint()

        # Close the default connection.
        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

    # Test bulk cursors on a fully restored database using live restore.
    def test_live_restore_complete_with_bulk(self):
        # Live restore is not supported on Windows.
        if os.name == 'nt':
            return

        self.populate_backup()

        os.mkdir("DEST")

        # Open live restore connection.
        self.open_conn("DEST", config="statistics=(all),live_restore=(enabled=true,path=\"SOURCE\",threads_max=1,read_size=512B)")

        self.wait_for_live_restore_complete()

        # Bulk load should be disabled for files migrated by live restore.
        msg = '/bulk-load is only supported on newly created objects/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor("file:bulk", None, "bulk"), msg)
