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

import fnmatch, os, shutil, threading, time
import wiredtiger, wttest
from wtbackup import backup_base
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint_snapshot04.py
#   Test utility dump of backup and original database when the transaction ids are
#   written to disk.
class test_checkpoint_snapshot04(backup_base):
    dir = 'backup.dir'

    # Create a table.
    uri = "table:test_checkpoint_snapshot04"
    nrows = 5000

    target_backup = [
        ('full', dict(target=False)),
        ('target', dict(target=True))
    ]

    scenarios = make_scenarios(target_backup)

    def conn_config(self):
        config = 'cache_size=200MB'
        return config

    def large_updates(self, uri, value, ds, nrows):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def check(self, check_value, uri, nrows):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)

    def test_checkpoint_snapshot(self):
        ds = SimpleDataSet(self, self.uri, 0, key_format="S", value_format="S")
        ds.populate()
        valuea = "aaaaa" * 100
        valueb = "bbbbb" * 100

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)
        for i in range(self.nrows, self.nrows + 1):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(valueb)
            self.assertEqual(cursor1.insert(), 0)

        self.large_updates(self.uri, valuea, ds, self.nrows)
        self.check(valuea, self.uri, self.nrows)

        self.session.checkpoint()

        # Create the backup directory.
        os.mkdir(self.dir)

        # Open up the backup cursor, and copy the files.
        if self.target:
            config = 'target=("table:test_checkpoint_snapshot04")'
        else:
            config = ""
        cursor = self.session.open_cursor('backup:', None, config)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            shutil.copy(cursor.get_key(), self.dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

        session1.rollback_transaction()

        self.compare_backups(self.uri, self.dir, './')
        # Due to unavailibility of history store file in targetted backup scenarios,
        # RTS doesn't get performed during the first restart, so compare the backup again
        # to confirm that RTS doesn't change the backup contents.
        self.compare_backups(self.uri, self.dir, './')

if __name__ == '__main__':
    wttest.run()
