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

import Queue
import threading, time, wiredtiger, wttest
import glob, os, shutil
from suite_subprocess import suite_subprocess
from wtscenario import check_scenarios
from wtthread import op_thread
from helper import compare_files, key_populate

# test_backup04.py
#    Utilities: wt backup
# Test cursor backup with target URIs
class test_backup_target(wttest.WiredTigerTestCase, suite_subprocess):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"

    # This test is written to test incremental backups.  We want to add
    # enough data to generate more than one log file each time we add data.
    # First we populate and take a full backup.  Then we loop, checkpointing
    # running an incremental backup with a targeted cursor and then calling
    # truncate to archive the logs.
    #
    # At the same time, we take a full backup during each loop.
    # We run recovery and verify the full backup with the incremental
    # backup after each loop.  We compare two backups instead of the original
    # because running 'wt' causes all of our original handles to be closed
    # and that is not what we want here.
    #
    pfx = 'test_backup'
    scenarios = check_scenarios([
        ('table', dict(uri='table:test',dsize=100,nops=2000,nthreads=1,time=30)),
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self, dir):
        return 'cache_size=1G,log=(archive=false,enabled,file_max=%s)' % \
            self.logmax

    def populate(self, uri, dsize, rows):
        self.pr('populate: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor[key_populate(cursor, i)] = str(i) + ':' + 'a' * dsize
        cursor.close()

    def update(self, uri, dsize, upd, rows):
        self.pr('update: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor[key_populate(cursor, i)] = str(i) + ':' + upd * dsize
        cursor.close()

    # Compare the original and backed-up files using the wt dump command.
    def compare(self, uri, dir_full, dir_incr):
        # print "Compare: full URI: " + uri + " with incremental URI "
        if dir_full == None:
            full_name='original'
        else:
            full_name='backup_full'
        incr_name='backup_incr'
        if os.path.exists(full_name):
            os.remove(full_name)
        if os.path.exists(incr_name):
            os.remove(incr_name)
        #
        # We have been copying the logs only, so we need to force 'wt' to
        # run recovery in order to apply all the logs and check the data.
        #
        if dir_full == None:
            self.runWt(['-R', 'dump', uri], outfilename=full_name)
        else:
            self.runWt(['-R', '-h', dir_full, 'dump', uri], outfilename=full_name)
        self.runWt(['-R', '-h', dir_incr, 'dump', uri], outfilename=incr_name)
        self.assertEqual(True,
            compare_files(self, full_name, incr_name))

    def take_full_backup(self, dir):
        # Open up the backup cursor, and copy the files.  Do a full backup.
        cursor = self.session.open_cursor('backup:', None, None)
        self.pr('Full backup to ' + dir + ': ')
        os.mkdir(dir)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            newfile = cursor.get_key()
            sz = os.path.getsize(newfile)
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + dir)
            shutil.copy(newfile, dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

    # Take an incremental backup and then truncate/archive the logs.
    def take_incr_backup(self, dir):
            config = 'target=("log:")'
            cursor = self.session.open_cursor('backup:', None, config)
            while True:
                ret = cursor.next()
                if ret != 0:
                    break
                newfile = cursor.get_key()
                sz = os.path.getsize(newfile)
                self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + dir)
                shutil.copy(newfile, dir)
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            self.session.truncate('log:', cursor, None, None)
            cursor.close()

    # Run background inserts while running checkpoints and incremental backups
    # repeatedly.
    def test_incremental_backup(self):
        import sys
        # Create the backup directory.
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.populate(self.uri, self.dsize, self.nops)

        # We need to start the directory for the incremental backup with
        # a full backup.  The full backup function creates the directory.
        dir = self.dir
        self.take_full_backup(dir)
        self.session.checkpoint(None)

        #
        # Incremental backups perform a loop:
        #   Do more work
        #   Checkpoint
        #   Copy log files returned by log targeted backup cursor
        #   Truncate (archive) the log files
        #   Close the backup cursor
        updstr="bcdefghi"
        for increment in range(0, 5):
            full_dir = self.dir + str(increment)
            # Add more work to move the logs forward.
            self.update(self.uri, self.dsize, updstr[increment], self.nops)
            self.session.checkpoint(None)

            self.pr('Iteration: ' + str(increment))
            self.take_incr_backup(self.dir)

        # After running, take a full backup.  Compare the incremental
        # backup to the original database and the full backup database.
        self.take_full_backup(full_dir)
        self.compare(self.uri, full_dir, self.dir)
        self.compare(self.uri, None, self.dir)


if __name__ == '__main__':
    wttest.run()
