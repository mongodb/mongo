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
from wtbackup import backup_base
from wtdataset import simple_key
from wtscenario import make_scenarios

# test_backup07.py
# Test cursor backup with target URIs, logging and create during backup.
class test_backup07(backup_base):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    newuri="table:newtable"

    pfx = 'test_backup'
    scenarios = make_scenarios([
        ('table', dict(uri='table:test',dsize=100,nthreads=1))
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'cache_size=1G,log=(enabled,file_max=%s,remove=false)' % \
            self.logmax

    # Run background inserts while running checkpoints repeatedly.
    def test_backup07(self):
        log2 = "WiredTigerLog.0000000002"

        self.session.create(self.uri, "key_format=S,value_format=S")

        # Insert small amounts of data at a time stopping just after we
        # cross into log file 2.
        while not os.path.exists(log2):
            self.add_data(self.uri, 'key', 'value')

        # Test a potential bug in full backups and creates.
        # We allow creates during backup because the file doesn't exist
        # when the backup metadata is created on cursor open and the newly
        # created file is not in the cursor list.

        # Create and add data to a new table and then copy the files with a full backup.
        os.mkdir(self.dir)

        # Now create and populate the new table. Make sure the log records
        # are on disk and will be copied to the backup.
        self.session.create(self.newuri, "key_format=S,value_format=S")
        self.add_data(self.newuri, 'key', 'value')
        self.session.log_flush('sync=on')

        # Now copy the files using full backup. This should not include the newly
        # created table.
        self.take_full_backup(self.dir)

        # After the full backup, open and recover the backup database.
        # Make sure we properly recover even though the log file will have
        # records for the newly created table file id.
        backup_conn = self.wiredtiger_open(self.dir)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
