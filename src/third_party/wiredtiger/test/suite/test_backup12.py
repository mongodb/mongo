#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

# test_backup12.py
# Test cursor backup with a block-based incremental cursor.
class test_backup12(wttest.WiredTigerTestCase, suite_subprocess):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:test"
    nops=100

    pfx = 'test_backup'

   #     ('archiving', dict(archive='true')),
   #     ('not-archiving', dict(archive='false')),
    scenarios = make_scenarios([
        ('archiving', dict(archive='true')),
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'cache_size=1G,log=(archive=%s,' % self.archive + \
            'enabled,file_max=%s)' % self.logmax

    def add_data(self):
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
        self.session.checkpoint()
        c.close()
        return loop

    def test_backup12(self):

        loop = self.add_data()

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        os.mkdir(self.dir)
        #
        # Note, this first backup is actually done before a checkpoint is taken.
        #
        config = 'incremental=(enabled,granularity=1M,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Add some data that will appear in log file 3.
        c = self.session.open_cursor(self.uri)
        for i in range(0, self.nops):
            num = i + (loop * self.nops)
            key = 'key' + str(num)
            val = 'value' + str(num)
            c[key] = val
        loop += 1
        c.close()
        self.session.log_flush('sync=on')
        self.session.checkpoint()

        # Now copy the files returned by the backup cursor.
        orig_logs = []
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            sz = os.path.getsize(newfile)
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            shutil.copy(newfile, self.dir)
            if "WiredTigerLog" in newfile:
                orig_logs.append(newfile)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        # Now open a duplicate backup cursor.
        # We *can* use a log target duplicate on an incremental primary backup so that
        # a backup process can get all the log files that occur while that primary cursor
        # is open.
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
        dupc.close()
        bkup_c.close()

        # Add more data.
        c = self.session.open_cursor(self.uri)
        for i in range(0, self.nops):
            num = i + (loop * self.nops)
            key = 'key' + str(num)
            val = 'value' + str(num)
            c[key] = val
        loop += 1
        c.close()
        self.session.log_flush('sync=on')
        self.session.checkpoint()

        # Now do an incremental backup.
        config = 'incremental=(src_id="ID1",this_id="ID2")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        self.pr('Open backup cursor ID1')
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            config = 'incremental=(file=' + newfile + ')'
            self.pr('Open incremental cursor with ' + config)
            dup_cnt = 0
            dupc = self.session.open_cursor(None, bkup_c, config)
            while True:
                ret = dupc.next()
                if ret != 0:
                    break
                incrlist = dupc.get_keys()
                offset = incrlist[0]
                size = incrlist[1]
                curtype = incrlist[2]
                self.assertTrue(curtype == 1 or curtype == 2)
                dup_cnt += 1
            dupc.close()
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            shutil.copy(newfile, self.dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()

        # After the full backup, open and recover the backup database.
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
