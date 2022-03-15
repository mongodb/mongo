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

import os, wiredtiger, wttest
from wtbackup import backup_base

# test_backup24.py
# Test recovering a selective backup with some logged tables, some not logged tables
# and creating more of each during backup.
class test_backup24(backup_base):
    dir='backup_all.dir' # Backup directory name
    config_log='key_format=S,value_format=S'
    config_nolog='key_format=S,value_format=S,log=(enabled=false)'
    log_t1="table:logged1"
    log_t2="table:logged2"
    log_tnew="table:loggednew"
    log_tnew_file="loggednew.wt"
    logmax="100K"
    nolog_t1="table:not1"
    nolog_t2="table:not2"
    nolog_t2_file="not2.wt"
    nolog_tnew="table:notnew"
    nolog_tnew_file="notnew.wt"
    newuri="table:newtable"

    def add_data(self, uri, key, val):
        c = self.session.open_cursor(uri, None, self.data_cursor_config)
        for i in range(0, self.nops):
            k = key + str(i)
            v = val + str(i)
            c[k] = v
        c.close()

    def check_data(self, uri, key, val):
        c = self.session.open_cursor(uri, None, self.data_cursor_config)
        for i in range(0, self.nops):
            c.set_key(key + str(i))
            self.assertEqual(c.search(), 0)
            self.assertEqual(c.get_value(), val + str(i))
        c.close()

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'debug_mode=(table_logging=true),cache_size=1G,log=(enabled,file_max=%s,remove=false)' % \
            self.logmax

    def test_backup24(self):
        log2 = "WiredTigerLog.0000000002"

        # Create two logged and two not-logged tables.
        self.session.create(self.log_t1, self.config_log)
        self.session.create(self.log_t2, self.config_log)
        self.session.create(self.nolog_t1, self.config_nolog)
        self.session.create(self.nolog_t2, self.config_nolog)

        # Insert small amounts of data at a time stopping just after we
        # cross into log file 2.
        while not os.path.exists(log2):
            self.add_data(self.log_t1, 'key', 'value')
            self.add_data(self.log_t2, 'key', 'value')
            self.add_data(self.nolog_t1, 'key', 'value')
            self.add_data(self.nolog_t2, 'key', 'value')

        self.session.checkpoint()
        # Add more data after the checkpoint.
        self.add_data(self.log_t1, 'newkey', 'newvalue')
        self.add_data(self.log_t2, 'newkey', 'newvalue')
        self.add_data(self.nolog_t1, 'newkey', 'newvalue')
        self.add_data(self.nolog_t2, 'newkey', 'newvalue')

        # We allow creates during backup because the file doesn't exist
        # when the backup metadata is created on cursor open and the newly
        # created file is not in the cursor list.

        # Create and add data to a new table and then copy the files with a full backup.
        os.mkdir(self.dir)

        # Open the backup cursor and then create new tables and add data to them.
        # Then copy the files.
        bkup_c = self.session.open_cursor('backup:', None, None)

        # Now create and populate the new table. Make sure the log records
        # are on disk and will be copied to the backup.
        self.session.create(self.log_tnew, self.config_log)
        self.session.create(self.nolog_tnew, self.config_nolog)
        self.add_data(self.log_tnew, 'key', 'value')
        self.add_data(self.nolog_tnew, 'key', 'value')
        self.session.log_flush('sync=on')

        # Now copy the files using full backup but as a selective backup. We want the logged
        # tables but only the first not-logged table. Skip the second not-logged table.
        all_files = self.take_selective_backup(self.dir, [self.nolog_t2_file], bkup_c)
        orig_logs = [file for file in all_files if "WiredTigerLog" in file]
        self.assertFalse(self.log_tnew in all_files)
        self.assertFalse(self.nolog_tnew in all_files)
        self.assertFalse(self.nolog_t2_file in all_files)

        # Take a log backup.
        self.take_log_backup(bkup_c, self.dir, orig_logs)
        bkup_c.close()

        target_uris = str([self.log_t1, self.log_t2, self.nolog_t1]).replace("\'", "\"")
        backup_conn = self.wiredtiger_open(self.dir, 'backup_restore_target={0}'.format(target_uris))
        flist = os.listdir(self.dir)
        self.assertFalse(self.nolog_t2_file in flist)
        self.assertFalse(self.nolog_tnew_file in flist)

        # Test the files we didn't copy over during selective backup don't exist in the metadata.
        bkup_session = backup_conn.open_session()
        metadata_c = bkup_session.open_cursor('metadata:', None, None)
        metadata_c.set_key(self.nolog_t2)
        self.assertEqual(metadata_c.search(), wiredtiger.WT_NOTFOUND)
        metadata_c.set_key(self.nolog_t2_file)
        self.assertEqual(metadata_c.search(), wiredtiger.WT_NOTFOUND)

        metadata_c.set_key(self.nolog_tnew)
        self.assertEqual(metadata_c.search(), wiredtiger.WT_NOTFOUND)
        metadata_c.set_key(self.nolog_tnew_file)
        self.assertEqual(metadata_c.search(), wiredtiger.WT_NOTFOUND)
        metadata_c.close()
        
        # Test that the database partial recovered successfully.
        self.check_data(self.log_t1, 'key', 'value')
        self.check_data(self.log_t2, 'key', 'value')
        self.check_data(self.nolog_t1, 'key', 'value')
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
