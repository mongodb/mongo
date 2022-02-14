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
from wtdataset import simple_key
from wtscenario import make_scenarios

# test_backup04.py
#    Utilities: wt backup
# Test incremental cursor backup.
class test_backup_target(backup_base):
    dir='backup.dir'                    # Backup directory name
    logmax="100K"

    # This test is written to test incremental backups.  We want to add
    # enough data to generate more than one log file each time we add data.
    # First we populate and take a full backup.  Then we loop, checkpointing
    # running an incremental backup with a targeted cursor and then calling
    # truncate to remove the logs.
    #
    # At the same time, we take a full backup during each loop.
    # We run recovery and verify the full backup with the incremental
    # backup after each loop.  We compare two backups instead of the original
    # because running 'wt' causes all of our original handles to be closed
    # and that is not what we want here.
    #
    pfx = 'test_backup'
    scenarios = make_scenarios([
        ('table', dict(uri='table:test',dsize=100,nops=2000,nthreads=1,time=30)),
    ])

    # Create a large cache, otherwise this test runs quite slowly.
    def conn_config(self):
        return 'cache_size=1G,log=(enabled,file_max=%s,remove=false)' % \
            self.logmax

    def populate_with_string(self, uri, dsize, rows):
        self.pr('populate: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor[simple_key(cursor, i)] = str(i) + ':' + 'a' * dsize
        cursor.close()

    def update(self, uri, dsize, upd, rows):
        self.pr('update: ' + uri + ' with ' + str(rows) + ' rows')
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, rows + 1):
            cursor[simple_key(cursor, i)] = str(i) + ':' + upd * dsize
        cursor.close()

    # Take an incremental backup and then truncate/remove the logs.
    def take_log_incr_backup(self, dir):
        config = 'target=("log:")'
        cursor = self.session.open_cursor('backup:', None, config)
        self.take_full_backup(dir, cursor)
        self.session.truncate('log:', cursor, None, None)
        cursor.close()

    # Run background inserts while running checkpoints and incremental backups
    # repeatedly.
    def test_log_incremental_backup(self):
        import sys
        # Create the backup directory.
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.populate_with_string(self.uri, self.dsize, self.nops)

        # We need to start the directory for the incremental backup with
        # a full backup.
        dir = self.dir
        os.mkdir(dir)
        self.take_full_backup(dir)
        self.session.checkpoint(None)

        #
        # Incremental backups perform a loop:
        #   Do more work
        #   Checkpoint
        #   Copy log files returned by log targeted backup cursor
        #   Truncate (remove) the log files
        #   Close the backup cursor
        updstr="bcdefghi"
        for increment in range(0, 5):
            # Add more work to move the logs forward.
            self.update(self.uri, self.dsize, updstr[increment], self.nops)
            self.session.checkpoint(None)

            self.pr('Iteration: ' + str(increment))
            self.take_log_incr_backup(self.dir)

        # After running, take a full backup.  Compare the incremental
        # backup to the original database and the full backup database.
        full_dir = self.dir + ".full"
        os.mkdir(full_dir)
        self.take_full_backup(full_dir)
        self.compare_backups(self.uri, self.dir, full_dir)
        self.compare_backups(self.uri, self.dir, './')

if __name__ == '__main__':
    wttest.run()
