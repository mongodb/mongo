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

import os
from wtbackup import backup_base
import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
try:
    # Windows does not getrlimit/setrlimit so we must catch the resource
    # module load.
    import resource
except:
    None

# test_backup06.py
#    Test that opening a backup cursor does not open file handles.
class test_backup06(backup_base):
    conn_config = 'statistics=(fast)'
    # This will create several hundred tables.
    num_table_sets = 10
    pfx='test_backup'

    # We try to do some schema operations.  Have some well
    # known names.
    schema_uri = 'file:schema_test'
    rename_uri = 'file:new_test'
    trename_uri = 'table:new_test'

    fobjs = [
        ( 'file:' + pfx + '.1', SimpleDataSet),
        ( 'file:' + pfx + '.2', SimpleDataSet),
    ]
    tobjs = [
        ('table:' + pfx + '.3', SimpleDataSet),
        ('table:' + pfx + '.4', SimpleDataSet),
        ('table:' + pfx + '.5', ComplexDataSet),
        ('table:' + pfx + '.6', ComplexDataSet),
        ('table:' + pfx + '.7', ComplexLSMDataSet),
        ('table:' + pfx + '.8', ComplexLSMDataSet),
    ]

    # Populate a set of objects.
    def populate_many(self):
        for t in range(0, self.num_table_sets):
            for i in self.fobjs:
                uri = i[0] + "." + str(t)
                i[1](self, uri, 10).populate()
            for i in self.tobjs:
                uri = i[0] + "." + str(t)
                i[1](self, uri, 10).populate()

    # Test that the open handle count does not change.
    def test_cursor_open_handles(self):
        if os.name == "nt":
            self.skipTest('Unix specific test skipped on Windows')

        limits = resource.getrlimit(resource.RLIMIT_NOFILE)
        if limits[0] < 1024:
            new = (1024, limits[1])
            resource.setrlimit(resource.RLIMIT_NOFILE, new)
        self.populate_many()
        # Close and reopen the connection so the populate dhandles are
        # not in the list.
        self.reopen_conn()

        new = (limits[0], limits[1])
        resource.setrlimit(resource.RLIMIT_NOFILE, new)

        # Confirm that opening a backup cursor does not open
        # file handles.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        dh_before = stat_cursor[stat.conn.dh_conn_handle_count][2]
        stat_cursor.close()
        cursor = self.session.open_cursor('backup:', None, None)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        dh_after = stat_cursor[stat.conn.dh_conn_handle_count][2]
        stat_cursor.close()
        if (dh_before != dh_after):
            print("Dhandles open before backup open: " + str(dh_before))
            print("Dhandles open after backup open: " + str(dh_after))
        self.assertEqual(dh_after == dh_before, True)
        cursor.close()

    def test_cursor_schema_protect(self):
        schema_uri = 'file:schema_test'
        rename_uri = 'file:new_test'
        trename_uri = 'table:new_test'

        #
        # Set up a number of tables.  Close and reopen the connection so that
        # we do not have open dhandles.  Then we want to open a backup cursor
        # testing both with and without the configuration setting.
        # We want to confirm that we open data handles when using schema
        # protection and we do not open the data handles when set to false.
        # We also want to make sure we detect and get an error when set to
        # false.  When set to true the open handles protect against schema
        # operations.
        self.populate(self.fobjs)
        self.populate(self.tobjs)
        cursor = self.session.open_cursor('backup:', None, None)
        # Check that we can create.
        self.session.create(schema_uri, None)
        for i in self.fobjs:
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.drop(i[0], None))
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.rename(i[0], rename_uri))
        for i in self.tobjs:
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.drop(i[0], None))
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.rename(i[0], trename_uri))
        cursor.close()

    # Test cursor reset runs through the list twice.
    def test_cursor_reset(self):
        self.populate(self.fobjs)
        self.populate(self.tobjs)
        cursor = self.session.open_cursor('backup:', None, None)
        i = 0
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            i += 1
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        total = i * 2
        cursor.reset()
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            i += 1
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(i, total)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
