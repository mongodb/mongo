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

# test_backup13.py
# Test cursor backup with a block-based incremental cursor and force_stop.
class test_backup13(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:test"
    nops=1000
    mult=0

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    def add_data(self, uri):

        c = self.session.open_cursor(uri)
        for i in range(0, self.nops):
            num = i + (self.mult * self.nops)
            key = self.bigkey + str(num)
            val = self.bigval + str(num)
            c[key] = val
        self.session.checkpoint()
        c.close()
        # Increase the multiplier so that later calls insert unique items.
        self.mult += 1

    def test_backup13(self):

        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data(self.uri)

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        os.mkdir(self.dir)
        config = 'incremental=(enabled,granularity=1M,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Add more data while the backup cursor is open.
        self.add_data(self.uri)

        # Now copy the files returned by the backup cursor.
        all_files = []

        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            sz = os.path.getsize(newfile)
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            shutil.copy(newfile, self.dir)
            all_files.append(newfile)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()

        # Add more data.
        self.add_data(self.uri)

        # Now do an incremental backup.
        config = 'incremental=(src_id="ID1",this_id="ID2")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        self.pr('Open backup cursor ID1')
        bkup_files = []
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            config = 'incremental=(file=' + newfile + ')'
            self.pr('Open incremental cursor with ' + config)
            dup_cnt = 0
            dupc = self.session.open_cursor(None, bkup_c, config)
            bkup_files.append(newfile)
            all_files.append(newfile)
            while True:
                ret = dupc.next()
                if ret != 0:
                    break
                incrlist = dupc.get_keys()
                offset = incrlist[0]
                size = incrlist[1]
                curtype = incrlist[2]
                self.assertTrue(curtype == wiredtiger.WT_BACKUP_FILE or curtype == wiredtiger.WT_BACKUP_RANGE)
                if curtype == wiredtiger.WT_BACKUP_FILE:
                    self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
                    shutil.copy(newfile, self.dir)
                else:
                    self.pr('Range copy file ' + newfile + ' offset ' + str(offset) + ' len ' + str(size))
                    rfp = open(newfile, "r+b")
                    wfp = open(self.dir + '/' + newfile, "w+b")
                    rfp.seek(offset, 0)
                    wfp.seek(offset, 0)
                    buf = rfp.read(size)
                    wfp.write(buf)
                    rfp.close()
                    wfp.close()
                dup_cnt += 1
            dupc.close()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()

        all_set = set(all_files)
        bkup_set = set(bkup_files)
        rem_files = list(all_set - bkup_set)
        for l in rem_files:
            self.pr('Remove file: ' + self.dir + '/' + l)
            os.remove(self.dir + '/' + l)
        # After the full backup, open and recover the backup database.
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

        # Do a force stop to release resources and reset the system.
        config = 'incremental=(force_stop=true)'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()

        # Make sure after a force stop we cannot access old backup info.
        config = 'incremental=(src_id="ID1",this_id="ID3")'
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))
        self.reopen_conn()
        # Make sure after a restart we cannot access old backup info.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))

if __name__ == '__main__':
    wttest.run()
