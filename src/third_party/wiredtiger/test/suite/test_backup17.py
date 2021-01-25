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
from wtbackup import backup_base
from wtdataset import simple_key
from wtscenario import make_scenarios

# test_backup17.py
# Test cursor backup with a block-based incremental cursor and consolidate.
class test_backup17(backup_base):
    dir='backup.dir'                    # Backup directory name
    gran="100K"
    granval=100*1024
    logmax="100K"
    uri="table:test"
    uri2="table:test2"

    conn_config='cache_size=1G,log=(enabled,file_max=%s)' % logmax

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    nops = 1000

    def take_incr_backup(self, id, consolidate):
        # Open the backup data source for incremental backup.
        buf = 'incremental=(src_id="ID' +  str(id - 1) + '",this_id="ID' + str(id) + '"'
        if consolidate:
            buf += ',consolidate=true'
        buf += ')'
        bkup_c = self.session.open_cursor('backup:', None, buf)
        lens = []
        saw_multiple = False
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
                # 1 is WT_BACKUP_FILE
                # 2 is WT_BACKUP_RANGE
                self.assertTrue(curtype == 1 or curtype == 2)
                if curtype == 1:
                    self.pr('Copy from: ' + newfile + ' (' + str(size) + ') to ' + self.dir)
                    shutil.copy(newfile, self.dir)
                else:
                    self.pr('Range copy file ' + newfile + ' offset ' + str(offset) + ' len ' + str(size))
                    lens.append(size)
                    rfp = open(newfile, "r+b")
                    wfp = open(self.dir + '/' + newfile, "w+b")
                    rfp.seek(offset, 0)
                    wfp.seek(offset, 0)
                    if size > self.granval:
                        saw_multiple = True
                    buf = rfp.read(size)
                    wfp.write(buf)
                    rfp.close()
                    wfp.close()
                dup_cnt += 1
            dupc.close()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()
        if consolidate:
            self.assertTrue(saw_multiple)
        else:
            self.assertFalse(saw_multiple)
        return lens

    def test_backup17(self):

        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.create(self.uri2, "key_format=S,value_format=S")
        self.add_data(self.uri, self.bigkey, self.bigval, True)
        self.mult = 0
        self.add_data(self.uri2, self.bigkey, self.bigval, True)

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        os.mkdir(self.dir)
        config = 'incremental=(enabled,granularity=%s,this_id="ID1")' % self.gran
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Now copy the files returned by the backup cursor.
        all_files = []
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

        # This is the main part of the test for consolidate. Add data to the first table.
        # Then perform the incremental backup with consolidate off (the default). Then add the
        # same data to the second table. Perform an incremental backup with consolidate on and
        # verify we get fewer, consolidated values.
        self.mult = 1
        self.add_data(self.uri, self.bigkey, self.bigval, True)

        uri1_lens = self.take_incr_backup(2, False)

        self.mult = 1
        self.add_data(self.uri2, self.bigkey, self.bigval, True)

        uri2_lens = self.take_incr_backup(3, True)

        # Assert that we recorded fewer lengths on the consolidated backup.
        self.assertLess(len(uri2_lens), len(uri1_lens))
        # Assert that we recorded the same total data length for both.
        self.assertEqual(sum(uri2_lens), sum(uri1_lens))

if __name__ == '__main__':
    wttest.run()
