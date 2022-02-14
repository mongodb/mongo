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
# test_backup09.py
#   Verify opening a backup cursor forces a log file switch.
#

import os, shutil
import helper, wiredtiger, wttest
from wtscenario import make_scenarios

class test_backup09(wttest.WiredTigerTestCase):
    # Have log writes go directly to the OS to avoid log_flush calls before
    # performing file copies not technically part of the backup cursor.
    conn_config = 'config_base=false,create,' \
        'log=(enabled),transaction_sync=(enabled,method=none)'
    uri = 'table:coll1'
    backup_dir = 'backup.dir'

    types = [
        # checkpoint: whether to explicitly checkpoint some data before opening
        #   the backup cursor.
        #
        # all_log_files: whether to copy all files in the source directory, or
        #   only the files returned from the backup cursor. Copying the
        #   additional log files will result in more operations being recovered.
        ('checkpoint', dict(checkpoint=True, all_log_files=False)),
        ('no_checkpoint', dict(checkpoint=False, all_log_files=False)),
        ('all_log_files', dict(checkpoint=True, all_log_files=True)),
    ]
    scenarios = make_scenarios(types)

    def data_and_start_backup(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')

        cursor = self.session.open_cursor(self.uri)
        doc_id = 0

        for i in range(10):
            doc_id += 1
            cursor[doc_id] = doc_id

        if self.checkpoint:
            self.session.checkpoint()

        for i in range(10):
            doc_id += 1
            cursor[doc_id] = doc_id

        last_doc_in_backup = doc_id
        self.assertEqual(1, len([x for x in os.listdir('.') if x.startswith('WiredTigerLog.')]))
        backup_cursor = self.session.open_cursor('backup:')
        self.assertEqual(2, len([x for x in os.listdir('.') if x.startswith('WiredTigerLog.')]))

        for i in range(10):
            doc_id += 1
            cursor[doc_id] = doc_id

        cursor.close()
        return backup_cursor, last_doc_in_backup, doc_id

    def copy_and_restore(self, backup_cursor, last_doc_in_backup, last_doc_in_data):
        log_files_to_copy = 0
        os.mkdir(self.backup_dir)
        if self.all_log_files:
            helper.copy_wiredtiger_home(self, '.', self.backup_dir)
            log_files_copied = [x for x in os.listdir(self.backup_dir) if x.startswith('WiredTigerLog.')]
            self.assertEqual(len(log_files_copied), 2)
        else:
            while True:
                ret = backup_cursor.next()
                if ret != 0:
                    break
                shutil.copy(backup_cursor.get_key(), self.backup_dir)
                if backup_cursor.get_key().startswith('WiredTigerLog.'):
                    log_files_to_copy += 1

            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            self.assertEqual(log_files_to_copy, 1)

        backup_conn = self.wiredtiger_open(self.backup_dir, self.conn_config)
        if self.all_log_files:
            self.captureout.checkAdditionalPattern(self, 'Both WiredTiger.turtle and WiredTiger.backup exist.*')

        session = backup_conn.open_session()
        cursor = session.open_cursor(self.uri)

        if self.all_log_files:
            doc_cnt = 0
            for key, val in cursor:
                doc_cnt += 1
                self.assertLessEqual(key, last_doc_in_data)

            self.assertEqual(doc_cnt, last_doc_in_data)
        else:
            doc_cnt = 0
            for key, val in cursor:
                doc_cnt += 1
                self.assertLessEqual(key, last_doc_in_backup)

            self.assertEqual(doc_cnt, last_doc_in_backup)

    def test_backup_rotates_log(self):
        if os.name == "nt" and self.all_log_files:
            self.skipTest('Unix specific test skipped on Windows')

        # Add some data, open a backup cursor, and add some more data. Return
        # the value of the last document that should appear on a restore.
        backup_cursor, last_doc_in_backup, last_doc_in_data = \
            self.data_and_start_backup()

        # Copy the files returned via the backup cursor and bring up WiredTiger
        # on the destination. Verify no document later than last_doc exists.
        self.copy_and_restore(
            backup_cursor, last_doc_in_backup, last_doc_in_data)

if __name__ == '__main__':
    wttest.run()
