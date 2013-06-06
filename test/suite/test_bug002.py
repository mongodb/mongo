#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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
# test_bug002.py
#       Regression tests.

import os
import shutil
import wiredtiger, wttest
from helper import key_populate, value_populate
from wtscenario import multiply_scenarios, number_scenarios

# Regression tests.
class test_bug002(wttest.WiredTigerTestCase):
    types = [
        ('file', dict(uri='file:data')),
        ('table', dict(uri='table:data')),
    ]
    name = [
        ('no', dict(name=0)),
        ('yes', dict(name=1)),
    ]

    scenarios = number_scenarios(multiply_scenarios('.', types, name))

    # Bulk-load handles return EBUSY to the checkpoint code, causing the
    # checkpoint call to find a handle anyway, and create fake checkpoint.
    # Named and unnamed checkpoint versions.
    def test_bulk_load_checkpoint(self):
        # Open a bulk cursor and insert a few records.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, 'bulk')
        for i in range(1, 10):
            cursor.set_key(key_populate(cursor, i))
            cursor.set_value(value_populate(cursor, i))
            cursor.insert()

        # Checkpoint a few times (to test the drop code).
        for i in range(1, 5):
            if self.name == 0:
                self.session.checkpoint()
            else:
                self.session.checkpoint('name=myckpt')

        # Close the bulk cursor.
        cursor.close()

        # In the case of named checkpoints, verify they're still there,
        # reflecting an empty file.
        if self.name == 1:
            cursor = self.session.open_cursor(
                self.uri, None, 'checkpoint=myckpt')
            self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)
            cursor.close()

    # Backup a set of chosen tables/files using the wt backup command.
    def backup_table_cursor(self, targetdir):
        # Remove any previous backup directories.
        shutil.rmtree(targetdir, True)
        os.mkdir(targetdir)

        # Open up the backup cursor, and copy the files.
        cursor = self.session.open_cursor('backup:', None, None)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            #print 'Copy from: ' + cursor.get_key() + ' to ' + targetdir
            shutil.copy(cursor.get_key(), targetdir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

        # Confirm the object we backed up exist, with correct contents.
        self.runWt(['dump', self.uri], outfilename='orig')
        self.runWt(['-h', targetdir, 'dump', self.uri], outfilename='backup')
        compare_files(self, 'orig', 'backup')

    def test_bulk_load_backup(self):
        # Open a bulk cursor and insert a few records.
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, 'bulk')
        for i in range(1, 10):
            cursor.set_key(key_populate(cursor, i))
            cursor.set_value(value_populate(cursor, i))
            cursor.insert()

        self.backup_table_cursor('backup.dir')

        # Close the bulk cursor.
        cursor.close()

if __name__ == '__main__':
    wttest.run()
