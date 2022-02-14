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
import wiredtiger
from wtbackup import backup_base
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_backup03.py
#    Utilities: wt backup
# Test cursor backup with target URIs
class test_backup_target(backup_base):
    dir='backup.dir'                    # Backup directory name

    # This test is written to test LSM hot backups: we test a simple LSM object
    # and a complex LSM object, but we can't test them both at the same time
    # because we need to load fast enough the merge threads catch up, and so we
    # test the real database, not what the database might look like after the
    # merging settles down.
    #
    # The way it works is we create 4 objects, only one of which is large, then
    # we do a hot backup of one or more of the objects and compare the original
    # to the backup to confirm the backup is correct.
    #
    # Set the chunk size small to avoid needing a lot of data when generating a
    # complex LSM tree.
    pfx = 'test_backup'
    objs = [                            # Objects
        ('table:' + pfx + '.1', SimpleDataSet, 0, ''),
        (  'lsm:' + pfx + '.2', SimpleDataSet, 1, ''),
        ('table:' + pfx + '.3', ComplexDataSet, 2, ''),
        ('table:' + pfx + '.4', ComplexLSMDataSet, 3, 'lsm=(chunk_size=512k)'),
    ]
    list = [
        ( 'backup_1', dict(big=0,list=[0])),       # Target objects individually
        ( 'backup_2', dict(big=1,list=[1])),
        ( 'backup_3', dict(big=2,list=[2])),
        ( 'backup_4', dict(big=3,list=[3])),
        ('backup_5a', dict(big=0,list=[0,2])),     # Target groups of objects
        ('backup_5b', dict(big=2,list=[0,2])),
        ('backup_6a', dict(big=1,list=[1,3])),
        ('backup_6b', dict(big=3,list=[1,3])),
        ('backup_7a', dict(big=0,list=[0,1,2])),
        ('backup_7b', dict(big=1,list=[0,1,2])),
        ('backup_7c', dict(big=2,list=[0,1,2])),
        ('backup_8a', dict(big=0,list=[0,1,2,3])),
        ('backup_8b', dict(big=1,list=[0,1,2,3])),
        ('backup_8c', dict(big=2,list=[0,1,2,3])),
        ('backup_8d', dict(big=3,list=[0,1,2,3])),
        ('backup_9', dict(big=3,list=[])),         # Backup everything
    ]

    scenarios = make_scenarios(list, prune=3, prunelong=1000)
    # Create a large cache, otherwise this test runs quite slowly.
    conn_config = 'cache_size=1G'

    # Backup a set of target tables using a backup cursor.
    def backup_table_cursor(self, l):
        # Create the backup directory.
        os.mkdir(self.dir)

        # Build the target list.
        config = ""
        if l:
                config = 'target=('
                for i in range(0, len(self.objs)):
                    if i in l:
                        config += '"' + self.objs[i][0] + '",'
                config += ')'

        # Open up the backup cursor, and copy the files.
        cursor = self.session.open_cursor('backup:', None, config)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            #print 'Copy from: ' + cursor.get_key() + ' to ' + self.dir
            shutil.copy(cursor.get_key(), self.dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

        # Confirm the objects we backed up exist, with correct contents.
        for i in range(0, len(self.objs)):
            if not l or i in l:
                self.compare_backups(self.objs[i][0], self.dir, './')

        # Confirm the other objects don't exist.
        if l:
            for i in range(0, len(self.objs)):
                if i not in l:
                    self.confirmPathDoesNotExist(self.objs[i][0], self.dir)

    # Test backup with targets.
    def test_backup_target(self):
        self.populate_big = self.big
        self.populate(self.objs, True)
        self.backup_table_cursor(self.list)

if __name__ == '__main__':
    wttest.run()
