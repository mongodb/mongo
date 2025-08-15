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


# test_live_restore06.py
# Ensure WiredTiger cleans all 'nbits=-1' strings from file metadata during backups.
class test_live_restore06(backup_base):

    def get_stat(self, statistic):
        stat_cursor = self.session.open_cursor("statistics:")
        val = stat_cursor[statistic][2]
        stat_cursor.close()
        return val

    def test_live_restore06(self):
        # FIXME-WT-14051 Live restore is not supported on Windows.
        if os.name == 'nt':
            return

        # Create an initial DB in SOURCE to be restored
        for i in range(0, 3):
            ds = SimpleDataSet(self, f'file:collection_{i}', 10_000,
            key_format='S', value_format='S')
            ds.populate()

        self.session.checkpoint()

        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

        # Start migration and set the sweep server to close files as quickly as possible.
        # We need the sweep server to close files after live restore transitions from
        # background migration to the clean up phase, but before we run the forced checkpoint at
        # the end of clean up. The timing stress flag live_restore_clean_up sleeps for 4 seconds
        # to give our 1 second sweep server enough time to close file handles.
        os.mkdir("DEST")
        self.reopen_conn("DEST",
            config="statistics=(all),live_restore=(enabled=true,path=\"SOURCE\",threads_max=1),\
                    file_manager=(close_idle_time=1,close_scan_interval=1,close_handle_minimum=1),\
                    timing_stress_for_test=[live_restore_clean_up]")

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

        # Once live restore has completed the metadata of all files will have nbits=-1
        meta_cursor = self.session.open_cursor('metadata:', None, None)
        while meta_cursor.next() != 0:
            uri = meta_cursor.get_key()
            if uri.find("file:") != -1:
                self.assertTrue("nbits=-1" in meta_cursor[uri])
        meta_cursor.close()

        # Now take a backup of the destination.
        # Test backups when live restore is in the COMPLETE phase
        self.do_backup_test("backup0","statistics=(all),live_restore=(enabled=true,path=\"SOURCE\")")
        # Test backups in non-live restore mode
        self.do_backup_test("backup1","statistics=(all),live_restore=(enabled=false)")

    def do_backup_test(self, backup_dir, backup_conn_config):
        self.reopen_conn(directory="DEST", config=backup_conn_config)
        os.mkdir(backup_dir)
        backup_cursor = self.session.open_cursor("backup:")
        self.take_full_backup(backup_dir, backup_cursor, home=self.conn.get_home())

        # Assert that backup/WiredTiger.backup doesn't contain the nbits=-1 string.
        # The backup file is plain text so we can simply grep for the string
        with open(backup_dir + "/WiredTiger.backup", "r") as f:
            self.assertTrue("nbits=-1" not in f.read())

        # Now open the backup and check all files have been cleaned and contain nbits=0.
        self.reopen_conn(directory=backup_dir, config="statistics=(all),live_restore=(enabled=false)")
        meta_cursor = self.session.open_cursor('metadata:', None, None)
        while meta_cursor.next() != 0:
            uri = meta_cursor.get_key()
            if uri.find("file:") != -1:
                self.assertTrue("nbits=0," in meta_cursor[uri])
        meta_cursor.close()
