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
from helper import simulate_crash_restart
from wtbackup import backup_base
from wtscenario import make_scenarios

# test_backup13.py
# Test cursor backup with a block-based incremental cursor and force_stop.
class test_backup13(backup_base):
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:test"

    scenarios = make_scenarios([
        ('default', dict(sess_cfg='')),
        ('read-committed', dict(sess_cfg='isolation=read-committed')),
        ('read-uncommitted', dict(sess_cfg='isolation=read-uncommitted')),
        ('snapshot', dict(sess_cfg='isolation=snapshot')),
    ])

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    nops = 1000

    def session_config(self):
        return self.sess_cfg

    def add_data_and_check(self):
        if self.sess_cfg == 'isolation=read-committed' or self.sess_cfg == 'isolation=read-uncommitted':
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.add_data(self.uri, self.bigkey, self.bigval, True),
                "/not supported in read-committed or read-uncommitted transactions/")
        else:
            self.add_data(self.uri, self.bigkey, self.bigval, True)

    def test_backup13(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data_and_check()

        os.mkdir(self.dir)

        # Add more data while the backup cursor is open.
        self.add_data_and_check()

        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        config = 'incremental=(enabled,granularity=1M,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Now make a full backup and track the files.
        all_files = self.take_full_backup(self.dir, bkup_c)
        bkup_c.close()
        # Add more data.
        self.add_data_and_check()

        # Now do an incremental backup with id 2.
        (bkup_files, _) = self.take_incr_backup(self.dir, 2)

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

        # Make sure after a crash we cannot access old backup info.
        simulate_crash_restart(self, ".", "RESTART")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))

        self.reopen_conn()
        # Make sure after a restart we cannot access old backup info.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, config))

if __name__ == '__main__':
    wttest.run()
