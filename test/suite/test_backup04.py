#!/usr/bin/env python
#
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
from wtscenario import multiply_scenarios, number_scenarios
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
    # We run recovery and verify the backup after reach incremental loop.
    pfx = 'test_backup'
    scenarios = [
        ('table', dict(uri='table:test',dsize=100,nops=2000,nthreads=1,time=30)),
    ]

    # Create a large cache, otherwise this test runs quite slowly.
    def setUpConnectionOpen(self, dir):
        wtopen_args = \
            'create,cache_size=1G,log=(archive=false,enabled,file_max=%s)' % \
            self.logmax
        conn = wiredtiger.wiredtiger_open(dir, wtopen_args)
        self.pr(`conn`)
        return conn

    def populate(self, uri, dsize, rows):
        self.pr('populate: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor.set_key(key_populate(cursor, i))
            my_data = str(i) + ':' + 'a' * dsize
            cursor.set_value(my_data)
            cursor.insert()
        cursor.close()

    def update(self, uri, dsize, upd, rows):
        self.pr('update: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor.set_key(key_populate(cursor, i))
            my_data = str(i) + ':' + upd * dsize
            cursor.set_value(my_data)
            cursor.insert()
        cursor.close()

    # Compare the original and backed-up files using the wt dump command.
    def compare(self, uri):
        print "Compare: URI: " + uri
        self.runWt(['dump', uri], outfilename='orig')
        print "Compare: Run recovery Backup: " + self.dir
        # Open the backup connection to force it to run recovery.
        backup_conn_params = \
            'log=(enabled,file_max=%s)' % self.logmax
        backup_conn = wiredtiger.wiredtiger_open(self.dir, backup_conn_params)
        # Allow threads to run
        backup_conn.close()
        print "Compare: Run wt dump on " + self.dir
        self.runWt(['-h', self.dir, 'dump', uri], outfilename='backup')
        print "Compare: compare files"
        self.assertEqual(True, compare_files(self, 'orig', 'backup'))

    # Run background inserts while running checkpoints and incremental backups
    # repeatedly.
    def test_incremental_backup(self):
        # Create the backup directory.
        os.mkdir(self.dir)
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.populate(self.uri, self.dsize, self.nops)

        # Open up the backup cursor, and copy the files.  Do a full backup.
        config = ""
        cursor = self.session.open_cursor('backup:', None, None)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            newfile = cursor.get_key()
            sz = os.path.getsize(newfile)
            print 'Copy from: ' + newfile + '(' + str(sz) + ') to ' + self.dir
            shutil.copy(newfile, self.dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()
        self.session.checkpoint(None)

        #
        # Incremental backups perform a loop:
        #   Do more work
        #   Checkpoint
        #   Copy log files returned by log targeted backup cursor
        #   Truncate (archive) the log files
        #   Close the backup cursor
        count = 5
        increment = 0
        updstr="bcdefghi"
        config = 'target=("log:")'
        while increment < count:
            # Add more work to move the logs forward.
            self.update(self.uri, self.dsize, updstr[increment], self.nops)
            self.session.checkpoint(None)
            cursor = self.session.open_cursor('backup:', None, config)
            print 'Iteration: ' + str(increment)
            while True:
                ret = cursor.next()
                if ret != 0:
                    break
                newfile = cursor.get_key()
                sz = os.path.getsize(newfile)
                print 'Copy from: ' + newfile + '(' + str(sz) + ') to ' + self.dir
                shutil.copy(newfile, self.dir)
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            self.session.truncate('log:', cursor, None, None)
            cursor.close()

            increment += 1
        print 'Done with backup loop'
        self.compare(self.uri)


if __name__ == '__main__':
    wttest.run()
