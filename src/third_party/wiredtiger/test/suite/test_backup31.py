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

import shutil, threading, time
import wiredtiger, wttest
from wtbackup import backup_base
from helper import copy_wiredtiger_home

# Helper thread to close the backup cursor in the background, allowing the
# main thread to copy files during the timing stress sleep.
class close_backup_thread(threading.Thread):
    def __init__(self, bkup_cursor):
        super().__init__()
        self.bkup_cursor = bkup_cursor

    def run(self):
        self.bkup_cursor.close()

# Test that log recovery correctly cleans up the backup ID string when
# replaying force stop records, preventing a memory leak.
class test_backup31(backup_base):
    conn_config = 'cache_size=50MB,log=(enabled,file_max=100K)'
    uri = "table:test_backup31"

    def test_backup_idstr_leak(self):
        # Create a table and add some data so recovery has something to run on.
        self.session.create(self.uri, "key_format=S,value_format=S")
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['key'] = 'value'
        cursor.close()
        self.session.checkpoint()

        # Open an incremental backup to register a backup ID.
        config = 'incremental=(enabled,this_id="LEAK_ID_TEST")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()

        # Configure checkpoint slow timing stress.
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_slow]')

        # Open with force_stop=true.
        config = 'incremental=(force_stop=true)'
        bkup_c2 = self.session.open_cursor('backup:', None, config)

        # Close the backup cursor in a background thread.
        # This triggers a checkpoint which writes the force-stop log record and sleeps.
        t = close_backup_thread(bkup_c2)
        t.start()

        # Sleep briefly to ensure the background thread has written the log record.
        time.sleep(1.0)

        # Copy the database files to simulate a crash before the checkpoint completes.
        copy_wiredtiger_home(self, ".", "RESTART")

        # Open the connection to the copied home directory.
        # This runs recovery, replaying both registration and force stop records.
        restart_conn = self.setUpConnectionOpen("RESTART")
        restart_session = self.setUpSessionOpen(restart_conn)
        restart_conn.close()

        # Wait for the background thread to finish its timing stress sleep.
        t.join()
