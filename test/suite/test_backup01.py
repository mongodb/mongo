#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import glob
import os
import shutil
import string
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from helper import compare_files,\
    complex_populate, complex_populate_lsm, simple_populate

# test_backup.py
#    Utilities: wt backup
# Test backup (both backup cursors and the wt backup command).
class test_backup(wttest.WiredTigerTestCase, suite_subprocess):
    dir='backup.dir'            # Backup directory name

    pfx = 'test_backup'
    objs = [
        ( 'file:' + pfx + '.1',  simple_populate, 0),
        ( 'file:' + pfx + '.2',  simple_populate, 0),
        ('table:' + pfx + '.3',  simple_populate, 0),
        ('table:' + pfx + '.4',  simple_populate, 0),
        ('table:' + pfx + '.5', complex_populate, 0),
        ('table:' + pfx + '.6', complex_populate, 0),
        ('table:' + pfx + '.7', complex_populate_lsm, 1),
        ('table:' + pfx + '.8', complex_populate_lsm, 1),
    ]

    # Populate a set of objects.
    def populate(self, skiplsm):
        for i in self.objs:
            if i[2]:
                if skiplsm:
                        continue
            i[1](self, i[0], 'key_format=S', 100)

    # Compare the original and backed-up files using the wt dump command.
    def compare(self, uri):
        self.runWt(['dump', uri], outfilename='orig')
        self.runWt(['-h', self.dir, 'dump', uri], outfilename='backup')
        self.assertEqual(True, compare_files(self, 'orig', 'backup'))

    # Test simple backup cursor open/close.
    def test_cursor_simple(self):
        cursor = self.session.open_cursor('backup:', None, None)
        cursor.close()

    # Test you can't have more than one backup cursor open at a time.
    def test_cursor_single(self):
        cursor = self.session.open_cursor('backup:', None, None)
        msg = '/there is already a backup cursor open/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None, None), msg)
        cursor.close()

    # Test backup of a database using the wt backup command.
    def test_backup_database(self):
        self.populate(0)
        os.mkdir(self.dir)
        self.runWt(['backup', self.dir])

        # Make sure all the files were copied.
        self.runWt(['list'], outfilename='outfile.orig')
        self.runWt(['-h', self.dir, 'list'], outfilename='outfile.backup')
        self.assertEqual(
            True, compare_files(self, 'outfile.orig', 'outfile.backup'))

        # And that the contents are the same.
        for i in self.objs:
            self.compare(i[0])

    # Check that a URI doesn't exist, both the meta-data and the file names.
    def confirmPathDoesNotExist(self, uri):
        conn = self.wiredtiger_open(self.dir)
        session = conn.open_session()
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session.open_cursor(uri, None, None))
        conn.close()

        self.assertEqual(
            glob.glob(self.dir + '*' + uri.split(":")[1] + '*'), [],
            'confirmPathDoesNotExist: URI exists, file name matching \"' +
            uri.split(":")[1] + '\" found')

    # Backup a set of chosen tables/files using the wt backup command.
    def backup_table(self, l):
        # Remove any previous backup directories.
        shutil.rmtree(self.dir, ignore_errors=True)
        os.mkdir(self.dir)

        # Build a command line of objects to back up and run wt.
        o = 'backup'
        for i in range(0, len(self.objs)):
            if i in l:
                o += ' -t ' + self.objs[i][0]
        o += ' ' + self.dir
        self.runWt(o.split())

        # Confirm the objects we backed up exist, with correct contents.
        for i in range(0, len(self.objs)):
            if i in l:
                self.compare(self.objs[i][0])

        # Confirm the other objects don't exist.
        for i in range(0, len(self.objs)):
            if i not in l:
                self.confirmPathDoesNotExist(self.objs[i][0])

    # Test backup of database subsets.
    def test_backup_table(self):
        self.populate(0)
        self.backup_table([0,2,4,6])
        self.backup_table([1,3,5,7])
        self.backup_table([0,1,2])
        self.backup_table([3,4,5])
        self.backup_table([5,6,7])

    # Test cursor reset runs through the list twice.
    def test_cursor_reset(self):
        self.populate(0)
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

    # Test that named checkpoints can't be deleted while backup cursors are
    # open, but that normal checkpoints continue to work.
    def test_checkpoint_delete(self):
        # You cannot name checkpoints including LSM tables, skip those.
        self.populate(1)

        # Confirm checkpoints are being deleted.
        self.session.checkpoint("name=one")
        self.session.checkpoint("name=two,drop=(one)")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.objs[0][0], None, "checkpoint=one"))

        # Confirm opening a backup cursor causes checkpoint to fail if dropping
        # a named checkpoint, but does not stop a default checkpoint.
        cursor = self.session.open_cursor('backup:', None, None)
        self.session.checkpoint()
        msg = '/checkpoints cannot be deleted during a hot backup/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("name=three,drop=(two)"), msg)
        self.session.checkpoint()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("name=three,drop=(two)"), msg)
        self.session.checkpoint()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
