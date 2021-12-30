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

from helper import copy_wiredtiger_home
from suite_subprocess import suite_subprocess
import os
import shutil
import wiredtiger, wttest

# test_backup.py
#   Test error message if verifying metadata on a backup restore.
class test_backup23(wttest.WiredTigerTestCase, suite_subprocess):
    '''Test backup, verifying metadata and an error opening the backup'''

    #conn_config = 'config_base=false,log=(archive=false,enabled),verbose=(recovery,log)'
    conn_config = 'config_base=false,log=(archive=false,enabled)'
    conn_config_err = 'config_base=false,log=(archive=false,enabled),verify_metadata=true'
    #conn_config_err = 'config_base=false,log=(archive=false,enabled),verify_metadata=true,verbose=(recovery,log)'
    dir='backup.dir'
    nentries = 10
    uri = 'file:backup.wt'

    def take_full_backup(self, dir):
        # Open up the backup cursor, and copy the files.  Do a full backup.
        cursor = self.session.open_cursor('backup:', None, None)
        self.pr('Full backup to ' + dir + ': ')
        os.mkdir(dir)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            bkup_file = cursor.get_key()
            sz = os.path.getsize(bkup_file)
            self.pr('Copy from: ' + bkup_file + ' (' + str(sz) + ') to ' + dir)
            shutil.copy(bkup_file, dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_backup23(self):
        '''Test backup, verifying metadata and an error opening the backup'''
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for i in range(self.nentries):
            c[i] = i
        self.session.commit_transaction()
        self.session.checkpoint()

        # Add more entries after the check point. They should be recovered.
        self.session.begin_transaction()
        for i in range(self.nentries):
            c[i + self.nentries] = i
        self.session.commit_transaction()
        c.close()
        orig_data = list(self.session.open_cursor(self.uri))

        # Take a full backup.
        self.take_full_backup(self.dir)
        self.close_conn()

        msg = '/restoring a backup is incompatible/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open(self.dir, self.conn_config_err), msg)

        self.pr('try opening error directory with correct config')
        # After getting the error we should be able to open the error backup directory with the
        # correct setting and then also see our data.
        self.conn = self.wiredtiger_open(self.dir, self.conn_config)
        session = self.conn.open_session()
        bkup_data = list(session.open_cursor(self.uri))

        self.assertEqual(orig_data, bkup_data)

if __name__ == '__main__':
    wttest.run()
