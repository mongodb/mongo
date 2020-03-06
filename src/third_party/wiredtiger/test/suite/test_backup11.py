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

# test_backup11.py
# Test cursor backup with a duplicate backup cursor.
class test_backup11(wttest.WiredTigerTestCase, suite_subprocess):
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

    def test_backup11(self):

        loop = self.add_data()

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        os.mkdir(self.dir)
        config = 'incremental=(enabled,this_id="ID1")'
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

        # Add more data
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

        # Test error cases now.

        # - Incremental filename must be on duplicate, not primary.
        # Test this first because we currently do not have a primary open.
        config = 'incremental=(file=test.wt)'
        msg = "/file name can only be specified on a duplicate/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor('backup:',
            None, config), 0), msg)

        # Open a non-incremental full backup cursor.
        # - An incremental duplicate must have an incremental primary.
        bkup_c = self.session.open_cursor('backup:', None, None)
        msg = "/must have an incremental primary/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)
        bkup_c.close()

        config = 'incremental=(src_id="ID1",this_id="ID2")'
        bkup_c = self.session.open_cursor('backup:', None, config)

        self.pr("Opened backup for error testing")

        # Now test all the error cases with an incremental primary open.
        # - We cannot make multiple incremental duplcate backup cursors.
        # - We cannot duplicate the duplicate backup cursor.
        config = 'incremental=(file=test.wt)'
        dupc = self.session.open_cursor(None, bkup_c, config)
        msg = "/already a duplicate backup cursor open/"
        self.pr("Test multiple dups")
        self.pr("=========")
        # Test multiple duplicate backup cursors.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)
        # Test duplicate of duplicate backup cursor.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            dupc, config), 0), msg)
        dupc.close()

        # - A duplicate cursor must specify incremental or log target.
        self.pr("Test dup and file target")
        self.pr("=========")
        msg = "/cannot be used for/"
        config = 'target=("file:test.wt")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)

        # - We cannot mix block incremental with a log target on the same duplicate.
        self.pr("Test mixed targets")
        self.pr("=========")
        config = 'incremental=(file=test.wt),target=("log:")'
        msg = "/incremental backup incompatible/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)

        # - Incremental ids must be on primary, not duplicate.
        self.pr("Test ids on dups")
        self.pr("=========")
        config = 'incremental=(src_id="ID1")'
        msg = "/specified on a primary/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)
        config = 'incremental=(this_id="ID1")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)

        # - Force stop must be on primary, not duplicate.
        #self.pr("Test force stop")
        #self.pr("=========")
        #config = 'incremental=(force_stop=true)'
        #print "config is " + config
        #self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
        #    lambda:self.assertEquals(self.session.open_cursor(None,
        #    bkup_c, config), 0), msg)

        bkup_c.close()

        # - Incremental must be opened on a primary with a source identifier.
        # Open a top level backup cursor without a source id.
        # Try to open an incremental cursor off this backup cursor.
        self.pr("Test incremental without source identifier on primary")
        self.pr("=========")
        config = 'incremental=(enabled,this_id="ID3")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        ret = bkup_c.next()
        self.assertTrue(ret == 0)
        newfile = bkup_c.get_key()
        config = 'incremental=(file=' + newfile + ')'
        msg = '/known source identifier/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(None, bkup_c, config), msg)
        bkup_c.close()

        # - Test opening a primary backup with an unknown source id.
        self.pr("Test incremental with unknown source identifier on primary")
        self.pr("=========")
        config = 'incremental=(enabled,src_id="ID_BAD",this_id="ID4")'
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))

        # - Test opening a primary backup with an id in WiredTiger namespace.
        self.pr("Test incremental with illegal src identifier using WiredTiger namespace")
        self.pr("=========")
        msg = '/name space may not/'
        config = 'incremental=(enabled,src_id="WiredTiger.0")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config), msg)

        # - Test opening a primary backup with an id in WiredTiger namespace.
        self.pr("Test incremental with illegal this identifier using WiredTiger namespace")
        self.pr("=========")
        config = 'incremental=(enabled,this_id="WiredTiger.ID")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config), msg)

        # - Test opening a primary backup with an id using illegal characters.
        self.pr("Test incremental with illegal source identifier using illegal colon character")
        self.pr("=========")
        msg = '/grouping characters/'
        config = 'incremental=(enabled,src_id="ID4:4.0")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config), msg)

        # - Test opening a primary backup with an id using illegal characters.
        self.pr("Test incremental with illegal this identifier using illegal colon character")
        self.pr("=========")
        config = 'incremental=(enabled,this_id="ID4:4.0")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config), msg)

        # - Test opening a primary backup with the same source id and this id (new id).
        self.pr("Test incremental with the same new source and this identifiers")
        self.pr("=========")
        config = 'incremental=(enabled,src_id="IDSAME",this_id="IDSAME")'
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))

        # - Test opening a primary backup with the same source id and this id (reusing id).
        self.pr("Test incremental with the same re-used source and this identifiers")
        self.pr("=========")
        msg = '/already in use/'
        config = 'incremental=(enabled,src_id="ID2",this_id="ID2")'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config), msg)

        # After the full backup, open and recover the backup database.
        #backup_conn = self.wiredtiger_open(self.dir)
        #backup_conn.close()

if __name__ == '__main__':
    wttest.run()
