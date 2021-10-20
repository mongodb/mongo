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
import os, shutil
from helper import compare_files
from suite_subprocess import suite_subprocess
from wtdataset import simple_key
from wtscenario import make_scenarios

# test_backup10.py
# Test cursor backup with a duplicate backup cursor.
class test_backup10(wttest.WiredTigerTestCase, suite_subprocess):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    newuri="table:newtable"
    uri="table:test"
    nops=100

    pfx = 'test_backup'

    scenarios = make_scenarios([
        ('archiving', dict(archive='true')),
        ('not-archiving', dict(archive='false')),
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'cache_size=1G,log=(archive=%s,' % self.archive + \
            'enabled,file_max=%s)' % self.logmax

    # Run background inserts while running checkpoints repeatedly.
    def test_backup10(self):
        log2 = "WiredTigerLog.0000000002"
        log3 = "WiredTigerLog.0000000003"

        self.session.create(self.uri, "key_format=S,value_format=S")

        # Insert small amounts of data at a time stopping after we
        # cross into log file 2.
        loop = 0
        c = self.session.open_cursor(self.uri)
        while not os.path.exists(log2):
            for i in range(0, self.nops):
                num = i + (loop * self.nops)
                key = 'key' + str(num)
                val = 'value' + str(num)
                c[key] = val
            loop += 1

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned.
        os.mkdir(self.dir)
        bkup_c = self.session.open_cursor('backup:', None, None)

        # Add some data that will appear in log file 3.
        for i in range(0, self.nops):
            num = i + (loop * self.nops)
            key = 'key' + str(num)
            val = 'value' + str(num)
            c[key] = val
        loop += 1
        c.close()
        self.session.log_flush('sync=on')

        # Now copy the files returned by the backup cursor.
        orig_logs = []
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            self.assertNotEqual(newfile, self.newuri)
            sz = os.path.getsize(newfile)
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            shutil.copy(newfile, self.dir)
            if "WiredTigerLog" in newfile:
                orig_logs.append(newfile)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        # Now open a duplicate backup cursor.
        config = 'target=("log:")'
        dupc = self.session.open_cursor(None, bkup_c, config)
        dup_logs = []
        while True:
            ret = dupc.next()
            if ret != 0:
                break
            newfile = dupc.get_key()
            self.assertTrue("WiredTigerLog" in newfile)
            sz = os.path.getsize(newfile)
            if (newfile not in orig_logs):
                self.pr('DUP: Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
                shutil.copy(newfile, self.dir)
            # Record all log files returned for later verification.
            dup_logs.append(newfile)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        # We expect that the duplicate logs are a superset of the
        # original logs. And we expect the difference to be the
        # addition of log file 3 only.
        orig_set = set(orig_logs)
        dup_set = set(dup_logs)
        self.assertTrue(dup_set.issuperset(orig_set))
        diff = dup_set.difference(orig_set)
        self.assertEqual(len(diff), 1)
        self.assertTrue(log3 in dup_set)

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
        msg = "/must be for logs/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, None), 0), msg)

        # Open duplicate backup cursor again now that the first
        # one is closed. Test every log file returned is the same
        # as the first time.
        dupc = self.session.open_cursor(None, bkup_c, config)
        while True:
            ret = dupc.next()
            if ret != 0:
                break
            newfile = dupc.get_key()
            self.assertTrue("WiredTigerLog" in newfile)
            self.assertTrue(newfile in dup_logs)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        dupc.close()
        bkup_c.close()

        # After the full backup, open and recover the backup database.
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
