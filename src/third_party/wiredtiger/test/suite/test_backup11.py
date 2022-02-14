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
#
# [TEST_TAGS]
# backup:cursors
# [END_TAGS]

import wiredtiger, wttest
import os
from wtbackup import backup_base

# test_backup11.py
# Test cursor backup with a duplicate backup cursor.
class test_backup11(backup_base):
    conn_config= 'cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    pfx = 'test_backup'
    uri="table:test"

    def test_backup11(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data(self.uri, 'key', 'value', True)
        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        os.mkdir(self.dir)
        config = 'incremental=(enabled,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Add data while the backup cursor is open.
        self.add_data(self.uri, 'key', 'value', True)

        # Now make a full backup and track the log files.
        all_files = self.take_full_backup(self.dir, bkup_c)
        orig_logs = [file for file in all_files if "WiredTigerLog" in file]

        # Now open a duplicate backup cursor.
        # We *can* use a log target duplicate on an incremental primary backup so that
        # a backup process can get all the log files that occur while that primary cursor
        # is open.
        dup_logs = self.take_log_backup(bkup_c, self.dir, orig_logs)
        bkup_c.close()

        # Add more data
        self.add_data(self.uri, 'key', 'value', True)

        # Test error cases now.

        # - Incremental filename must be on duplicate, not primary.
        # Test this first because we currently do not have a primary open.
        config = 'incremental=(file=test.wt)'
        msg = "/file name can only be specified on a duplicate/"
        self.pr("Specify file on primary")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor('backup:',
            None, config), 0), msg)

        # Open a non-incremental full backup cursor.
        # - An incremental duplicate must have an incremental primary.
        self.pr("Try to open an incremental on a non-incremental primary")
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
        # - We cannot specify consolidate on the duplicate cursor.
        config = 'incremental=(consolidate=true,file=test.wt)'
        msg = "/consolidation can only be specified on a primary/"
        self.pr("Test consolidation on a dup")
        self.pr("=========")
        # Test multiple duplicate backup cursors.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)

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
        self.pr("Test force stop")
        self.pr("=========")
        config = 'incremental=(force_stop=true)'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.open_cursor(None,
            bkup_c, config), 0), msg)

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
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
