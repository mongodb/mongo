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

# test_backup30.py
# Test querying the checkpoint timestamp for a backup cursor.
class test_backup30(backup_base):
    dir='backup.dir'                    # Backup directory name
    uri="table:table30"

    def add_timestamp_data(self, uri, key, val, timestamp):
        self.session.begin_transaction()
        c = self.session.open_cursor(uri, None, None)
        for i in range(0, 1000):
            k = key + str(i)
            v = val
            c[k] = v
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))

    def test_backup30(self):
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.add_timestamp_data(self.uri, "key", "val", 1)
        self.add_timestamp_data(self.uri, "key", "val5", 5)
        # Stable timestamp at 10.
        stable = self.timestamp_str(10)
        self.conn.set_timestamp('stable_timestamp=' + stable)
        self.session.checkpoint()
        # If a backup cursor isn't open the returned timestamp should be 0.
        self.assertTimestampsEqual("0", self.conn.query_timestamp('get=backup_checkpoint'))
        # Open a backup cursor.
        bkup_c = self.session.open_cursor('backup:', None, None)
        # If a backup cursor is open the returned timestamp should be the checkpoint stable.
        self.assertTimestampsEqual(stable, self.conn.query_timestamp('get=backup_checkpoint'))

        # Add more data and advance stable and take another checkpoint while the backup
        # cursor is open. Make sure backup timestamp remains the same.
        self.add_timestamp_data(self.uri, "key", "val11", 11)
        self.add_timestamp_data(self.uri, "key", "val15", 15)
        stable2 = self.timestamp_str(20)
        self.conn.set_timestamp('stable_timestamp=' + stable2)
        self.session.checkpoint()

        self.assertTimestampsEqual(stable, self.conn.query_timestamp('get=backup_checkpoint'))

        # Close and reopen the backup cursor and make sure it advances.
        bkup_c.close()
        self.assertTimestampsEqual("0", self.conn.query_timestamp('get=backup_checkpoint'))
        bkup_c = self.session.open_cursor('backup:', None, None)
        # If a backup cursor is open the returned timestamp should be the checkpoint stable.
        self.assertTimestampsEqual(stable2, self.conn.query_timestamp('get=backup_checkpoint'))
