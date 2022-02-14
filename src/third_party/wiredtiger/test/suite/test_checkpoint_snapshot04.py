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

import os, shutil
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

    format_values = [
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('row_string', dict(key_format='S', value_format='S')),
    ]

    target_backup = [
        ('full', dict(target=False)),
        ('target', dict(target=True))
    ]

    scenarios = make_scenarios(format_values, target_backup)

    def conn_config(self):
        config = 'cache_size=200MB'
        return config

    def large_updates(self, uri, value, ds, nrows):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def check(self, check_value, uri, nrows):
        # In FLCS the existence of the invisible extra row causes the table to extend
        # under it. Until that's fixed, expect (not just allow) this row to exist and
        # and demand it reads back as zero and not as check_value. When this behavior
        # is fixed (so the end of the table updates transactionally) the special-case
        # logic can just be removed.
        flcs_tolerance = self.value_format == '8t'

        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if flcs_tolerance and count >= nrows:
                self.assertEqual(v, 0)
            else:
                self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows + 1 if flcs_tolerance else nrows)

    def test_checkpoint_snapshot(self):
        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)
        for i in range(self.nrows + 1, self.nrows + 2):
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
