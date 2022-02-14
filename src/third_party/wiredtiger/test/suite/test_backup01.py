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
# [TEST_TAGS]
# wt_util
# backup:cursors
# [END_TAGS]
#

import os
import shutil
import time
from wtbackup import backup_base
import wiredtiger
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from helper import compare_files

# test_backup.py
#    Utilities: wt backup
# Test backup (both backup cursors and the wt backup command).
class test_backup(backup_base):
    dir='backup.dir'            # Backup directory name

    pfx = 'test_backup'
    objs = [
        ( 'file:' + pfx + '.1', SimpleDataSet, 0),
        ( 'file:' + pfx + '.2', SimpleDataSet, 0),
        ('table:' + pfx + '.3', SimpleDataSet, 0),
        ('table:' + pfx + '.4', SimpleDataSet, 0),
        ('table:' + pfx + '.5', ComplexDataSet, 0),
        ('table:' + pfx + '.6', ComplexDataSet, 0),
        ('table:' + pfx + '.7', ComplexLSMDataSet, 1),
        ('table:' + pfx + '.8', ComplexLSMDataSet, 1),
    ]

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
        self.populate(self.objs)
        os.mkdir(self.dir)
        self.runWt(['backup', self.dir])

        # Make sure all the files were copied.
        self.runWt(['list'], outfilename='outfile.orig')
        self.runWt(['-h', self.dir, 'list'], outfilename='outfile.backup')
        self.assertEqual(
            True, compare_files(self, 'outfile.orig', 'outfile.backup'))

        # And that the contents are the same.
        for i in self.objs:
            self.compare_backups(i[0], self.dir, './')

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
                self.compare_backups(self.objs[i][0], self.dir, './')

        # Confirm the other objects don't exist.
        for i in range(0, len(self.objs)):
            if i not in l:
                self.confirmPathDoesNotExist(self.objs[i][0], self.dir)

    # Test backup of database subsets.
    def test_backup_table(self):
        self.populate(self.objs)
        self.backup_table([0,2,4,6])
        self.backup_table([1,3,5,7])
        self.backup_table([0,1,2])
        self.backup_table([3,4,5])
        self.backup_table([5,6,7])

    # Test cursor reset runs through the list twice.
    def test_cursor_reset(self):
        self.populate(self.objs)
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

    # Test interaction between checkpoints and a backup cursor.
    def test_checkpoint_delete(self):
        # You cannot name checkpoints including LSM tables, skip those.
        self.populate(self.objs, False, True)

        # Confirm checkpoints are being deleted.
        self.session.checkpoint("name=one")
        self.session.checkpoint("name=two,drop=(one)")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.objs[0][0], None, "checkpoint=one"))

        # Confirm opening a backup cursor causes checkpoint to fail if dropping
        # a named checkpoint created before the backup cursor, but does not stop a
        # default checkpoint.
        cursor = self.session.open_cursor('backup:', None, None)
        self.session.checkpoint()
        msg = '/checkpoints cannot be deleted during a hot backup/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("name=three,drop=(two)"), msg)
        self.session.checkpoint()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint("name=three,drop=(two)"), msg)
        self.session.checkpoint()

        # Need to pause a couple seconds; checkpoints that are assigned the same timestamp as
        # the backup will be pinned, even if they occur after the backup starts.
        time.sleep(2)

        # Confirm that a named checkpoint created after a backup cursor can be dropped.
        self.session.checkpoint("name=four")
        self.session.checkpoint("drop=(four)")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.objs[0][0], None, "checkpoint=four"))

        # Confirm that after closing the backup cursor the original named checkpoint can
        # be deleted.
        cursor.close()
        self.session.checkpoint("drop=(two)")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            self.objs[0][0], None, "checkpoint=two"))

if __name__ == '__main__':
    wttest.run()
