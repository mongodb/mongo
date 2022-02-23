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

import wiredtiger, wttest
import os
from helper import copy_wiredtiger_home
from wtbackup import backup_base

# test_backup25.py
# Test commit-level durability, logging and later checkpoints when a backup
# cursor is open. If a crash occurs on a source while a backup cursor is open
# WiredTiger restarts and recovers from the checkpoint prior to the backup
# cursor, therefore discarding the later checkpoint. The reason is that if a
# WiredTiger.backup file exists on restart, then it is used. Verify that commit
# level durability is applies after a crash.
class test_backup25(backup_base):
    config_log='key_format=S,value_format=S'
    dir = "newdir"
    uri="table:backup25"
    logmax="100K"

    # Create a large cache.
    def conn_config(self):
        return 'cache_size=1G,log=(enabled,file_max=%s)' % \
            self.logmax

    # Run background inserts while running checkpoints repeatedly.
    def test_backup25(self):
        log2 = "WiredTigerLog.0000000002"

        # Create a logged table.
        self.session.create(self.uri, self.config_log)

        # Insert small amounts of data at a time stopping just after we
        # cross into log file 2.
        while not os.path.exists(log2):
            self.add_data(self.uri, 'key', 'value')

        self.session.checkpoint()
        # Add more data after the checkpoint.
        c = self.session.open_cursor(self.uri)
        c["newkey"] = "newvalue"
        c.close()

        # Open the backup cursor and then add new data to the table.
        bkup_c = self.session.open_cursor('backup:', None, None)

        # Add new data twice and checkpoint to have later checkpoints after the backup
        # cursor is open. Add an uncheckpointed but logged modification too.
        c = self.session.open_cursor(self.uri)
        c["bkupkey1"] = "bkupvalue1"
        c.close()
        self.session.checkpoint()
        c = self.session.open_cursor(self.uri)
        c["bkupkey2"] = "bkupvalue2"
        c.close()
        self.session.checkpoint()
        c = self.session.open_cursor(self.uri)
        c["bkupkey3"] = "bkupvalue3"
        c.close()

        # Make sure any data log records are on disk.
        self.session.log_flush('sync=on')

        # Make a copy of the database to another directory to restart after the "crash"
        # with the backup cursor open.
        os.mkdir(self.dir)
        copy_wiredtiger_home(self, '.', self.dir)
        bkup_c.close()

        # Open the new directory and verify we can see the data after the backup cursor was opened.
        with self.expectedStdoutPattern('Both WiredTiger.turtle and WiredTiger.backup exist.*'):
            new_conn = self.wiredtiger_open(self.dir)
        new_sess = new_conn.open_session()
        c = new_sess.open_cursor(self.uri)
        self.assertEqual(c["bkupkey1"], "bkupvalue1")
        self.assertEqual(c["bkupkey2"], "bkupvalue2")
        self.assertEqual(c["bkupkey3"], "bkupvalue3")
        c.close()

        new_conn.close()

if __name__ == '__main__':
    wttest.run()
