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
from wtbackup import backup_base
from wtscenario import make_scenarios

# test_backup10.py
# Test cursor backup with a duplicate backup cursor.
class test_backup10(backup_base):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:test"

    pfx = 'test_backup'

    scenarios = make_scenarios([
        ('removing', dict(remove='true')),
        ('not-removing', dict(remove='false')),
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'cache_size=1G,log=(remove=%s,' % self.remove + \
            'enabled,file_max=%s)' % self.logmax

    # Run background inserts while running checkpoints repeatedly.
    def test_backup10(self):
        log2 = "WiredTigerLog.0000000002"
        log3 = "WiredTigerLog.0000000003"
        log4 = "WiredTigerLog.0000000004"

        self.session.create(self.uri, "key_format=S,value_format=S")

        # Insert small amounts of data at a time stopping after we
        # cross into log file 2.
        while not os.path.exists(log2):
            self.add_data(self.uri, 'key', 'value')

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned.
        os.mkdir(self.dir)
        bkup_c = self.session.open_cursor('backup:', None, None)

        # Add some data that will appear in log file 3.
        self.add_data(self.uri, 'key', 'value')
        self.session.log_flush('sync=on')

        # Now make a full backup and track the log files.
        all_files = self.take_full_backup(self.dir, bkup_c)
        orig_logs = [file for file in all_files if "WiredTigerLog" in file]

        # Now open a duplicate backup cursor.
        config = 'target=("log:")'
        dupc = self.session.open_cursor(None, bkup_c, config)
        dup_logs = self.take_log_backup(bkup_c, self.dir, orig_logs, dupc)

        # We expect that the duplicate logs are a superset of the
        # original logs. And we expect the difference to be the
        # addition of two log files, one switch when opening the backup
        # cursor and a switch when opening the duplicate cursor.
        orig_set = set(orig_logs)
        dup_set = set(dup_logs)
        self.assertTrue(dup_set.issuperset(orig_set))
        diff = dup_set.difference(orig_set)
        self.assertEqual(len(diff), 1)
        self.assertTrue(log3 in dup_set)
        self.assertFalse(log3 in orig_set)
        self.assertFalse(log4 in dup_set)

        # Test a few error cases now.
        # - We cannot make multiple duplcate backup cursors.
        # - We cannot duplicate the duplicate backup cursor.
        # - We must use the log target.
        msg = "/already a duplicate backup cursor open/"
        # Test multiple duplicate backup cursors.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)
        # Test duplicate of duplicate backup cursor.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            dupc, config), 0), msg)
        dupc.close()

        # Test we must use the log target.
        msg = "/must be for /"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, None), 0), msg)

        bkup_c.close()

        # After the full backup, open and recover the backup database.
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
