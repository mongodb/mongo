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
from wtscenario import make_scenarios

# test_backup27.py
# Test selective backup with history store contents. Recovering a partial backup should 
# clear the history entries of the table that does not exist in the backup directory.
class test_backup27(backup_base):
    dir='backup.dir'                    # Backup directory name
    newuri="table:table_no_hs"
    newuri_file="table_no_hs.wt"
    uri="table:table_hs"

    def add_timestamp_data(self, uri, key, val, timestamp):
        self.session.begin_transaction()
        c = self.session.open_cursor(uri, None, None)
        for i in range(0, 1000):
            k = key + str(i)
            v = val
            c[k] = v
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))

    def validate_timestamp_data(self, session, uri, key, expected_val, expected_err, timestamp):
        session.begin_transaction('read_timestamp=' + self.timestamp_str(timestamp))
        c = session.open_cursor(uri, None, None)
        for i in range(0, 1000):
            k = key + str(i)
            c.set_key(k)
            self.assertEqual(c.search(), expected_err)
            if (expected_err == 0):
                self.assertEqual(c.get_value(), expected_val)
        c.close()
        session.commit_transaction()

    def test_backup27(self):
        log2 = "WiredTigerLog.0000000002"

        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.create(self.newuri, "key_format=S,value_format=S")

        self.add_timestamp_data(self.uri, "key", "val", 1)
        self.add_timestamp_data(self.newuri, "key", "val", 1)

        self.add_timestamp_data(self.uri, "key", "val5", 5)
        self.add_timestamp_data(self.newuri, "key", "val5", 5)

        # Stable timestamp at 10, so that we can retain history store data.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        os.mkdir(self.dir)

        # Now copy the files using selective backup. This should not include one of the tables.
        all_files = self.take_selective_backup(self.dir, [self.newuri_file])

        # After the full backup, open and partially recover the backup database on only one table.
        backup_conn = self.wiredtiger_open(self.dir, "backup_restore_target=[\"{0}\"]".format(self.uri))
        bkup_session = backup_conn.open_session()

        # Test that the history store data still exists for the tables that got restored.
        self.validate_timestamp_data(bkup_session, self.uri, "key", "val", 0, 1)
        self.validate_timestamp_data(bkup_session, self.uri, "key", "val5", 0, 10)

        # Open the cursor and expect failure since file doesn't exist.
        self.assertRaisesException(
             wiredtiger.WiredTigerError,lambda: bkup_session.open_cursor(self.newuri, None, None))
        bkup_session.create(self.newuri, "key_format=S,value_format=S")
        self.validate_timestamp_data(bkup_session, self.newuri, "key", None, wiredtiger.WT_NOTFOUND, 1)
        self.validate_timestamp_data(bkup_session, self.newuri, "key", None, wiredtiger.WT_NOTFOUND, 10)
        backup_conn.close()

if __name__ == '__main__':
    wttest.run()
