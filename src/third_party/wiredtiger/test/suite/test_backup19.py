#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

import wiredtiger, wttest
import os, shutil
from helper import compare_files
from suite_subprocess import suite_subprocess
from wtdataset import simple_key
from wtscenario import make_scenarios
import glob

# test_backup19.py
# Test cursor backup with a block-based incremental cursor source id only.
class test_backup19(wttest.WiredTigerTestCase, suite_subprocess):
    bkp_home = "WT_BLOCK"
    counter=0
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    logmax="100K"
    mult=0
    nops=10000
    savefirst=0
    savekey='NOTSET'
    uri="table:main"

    dir='backup.dir'                    # Backup directory name
    home_full = "WT_BLOCK_LOG_FULL"
    home_incr = "WT_BLOCK_LOG_INCR"

    full_out = "./backup_block_full"
    incr_out = "./backup_block_incr"
    logpath = "logpath"
    new_table=False
    initial_backup=False

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    #
    # Set up all the directories needed for the test. We have a full backup directory for each
    # iteration and an incremental backup for each iteration. That way we can compare the full and
    # incremental each time through.
    #
    def setup_directories(self):
        # We're only coming through once so just set up the 0 and 1 directories.
        for i in range(0, 2):
            # The log directory is a subdirectory of the home directory,
            # creating that will make the home directory also.
            log_dir = self.home_incr + '.' + str(i) + '/' + self.logpath
            os.makedirs(log_dir)
            if i != 0:
                log_dir = self.home_full + '.' + str(i) + '/' + self.logpath
                os.makedirs(log_dir)

    def range_copy(self, filename, offset, size):
        read_from = filename
        old_to = self.home_incr + '.' + str(self.counter - 1) + '/' + filename
        write_to = self.home_incr + '.' + str(self.counter) + '/' + filename
        rfp = open(read_from, "r+b")
        self.pr('RANGE CHECK file ' + old_to + ' offset ' + str(offset) + ' len ' + str(size))
        rfp2 = open(old_to, "r+b")
        rfp.seek(offset, 0)
        rfp2.seek(offset, 0)
        buf = rfp.read(size)
        buf2 = rfp2.read(size)
        # This assertion tests that the offset range we're given actually changed
        # from the previous backup.
        self.assertNotEqual(buf, buf2)
        wfp = open(write_to, "w+b")
        wfp.seek(offset, 0)
        wfp.write(buf)
        rfp.close()
        rfp2.close()
        wfp.close()

    def take_full_backup(self):
        if self.counter != 0:
            hdir = self.home_full + '.' + str(self.counter)
        else:
            hdir = self.home_incr

        #
        # First time through we take a full backup into the incremental directories. Otherwise only
        # into the appropriate full directory.
        #
        buf = None
        if self.initial_backup == True:
            buf = 'incremental=(granularity=1M,enabled=true,this_id=ID0)'

        bkup_c = self.session.open_cursor('backup:', None, buf)
        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()

            if self.counter == 0:
                # Take a full backup into each incremental directory
                for i in range(0, 2):
                    copy_from = newfile
                    # If it is a log file, prepend the path.
                    if ("WiredTigerLog" in newfile):
                        copy_to = self.home_incr + '.' + str(i) + '/' + self.logpath
                    else:
                        copy_to = self.home_incr + '.' + str(i)
                    shutil.copy(copy_from, copy_to)
            else:
                copy_from = newfile
                # If it is log file, prepend the path.
                if ("WiredTigerLog" in newfile):
                    copy_to = hdir + '/' + self.logpath
                else:
                    copy_to = hdir

                shutil.copy(copy_from, copy_to)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()

    def take_incr_backup(self):
        self.assertTrue(self.counter > 0)
        # Open the backup data source for incremental backup.
        buf = 'incremental=(src_id="ID' +  str(self.counter - 1) + '")'
        self.pr(buf)
        bkup_c = self.session.open_cursor('backup:', None, buf)

        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            newfile = bkup_c.get_key()
            h = self.home_incr + '.0'
            copy_from = newfile
            # If it is log file, prepend the path.
            if ("WiredTigerLog" in newfile):
                copy_to = h + '/' + self.logpath
            else:
                copy_to = h

            shutil.copy(copy_from, copy_to)
            first = True
            config = 'incremental=(file=' + newfile + ')'
            dup_cnt = 0
            # For each file listed, open a duplicate backup cursor and copy the blocks.
            incr_c = self.session.open_cursor(None, bkup_c, config)

            # We cannot use 'for newfile in incr_c:' usage because backup cursors don't have
            # values and adding in get_values returns ENOTSUP and causes the usage to fail.
            # If that changes then this, and the use of the duplicate below can change.
            while True:
                ret = incr_c.next()
                if ret != 0:
                    break
                incrlist = incr_c.get_keys()
                offset = incrlist[0]
                size = incrlist[1]
                curtype = incrlist[2]
                self.assertTrue(curtype == wiredtiger.WT_BACKUP_FILE or curtype == wiredtiger.WT_BACKUP_RANGE)
                if curtype == wiredtiger.WT_BACKUP_FILE:
                    # Copy the whole file.
                    if first == True:
                        h = self.home_incr + '.' + str(self.counter)
                        first = False

                    copy_from = newfile
                    if ("WiredTigerLog" in newfile):
                        copy_to = h + '/' + self.logpath
                    else:
                        copy_to = h
                    shutil.copy(copy_from, copy_to)
                else:
                    # Copy the block range.
                    self.pr('Range copy file ' + newfile + ' offset ' + str(offset) + ' len ' + str(size))
                    self.range_copy(newfile, offset, size)
                dup_cnt += 1
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            incr_c.close()

            # For each file, we want to copy it into each of the later incremental directories.
            for i in range(self.counter, 2):
                h = self.home_incr + '.' + str(i)
                copy_from = newfile
                if ("WiredTigerLog" in newfile):
                    copy_to = h + '/' + self.logpath
                else:
                    copy_to = h
                shutil.copy(copy_from, copy_to)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_c.close()

    def compare_backups(self, t_uri):
        # Run wt dump on full backup directory.
        full_backup_out = self.full_out + '.' + str(self.counter)
        home_dir = self.home_full + '.' + str(self.counter)
        if self.counter == 0:
            home_dir = self.home
        self.runWt(['-R', '-h', home_dir, 'dump', t_uri], outfilename=full_backup_out)

        # Run wt dump on incremental backup directory.
        incr_backup_out = self.incr_out + '.' + str(self.counter)
        home_dir = self.home_incr + '.' + str(self.counter)
        self.runWt(['-R', '-h', home_dir, 'dump', t_uri], outfilename=incr_backup_out)

        self.assertEqual(True,
            compare_files(self, full_backup_out, incr_backup_out))

    #
    # Add data to the given uri.
    #
    def add_data(self, uri):
        c = self.session.open_cursor(uri, None, None)
        # The first time we want to add in a lot of data. Then after that we want to
        # rapidly change a single key to create a hotspot in one block.
        if self.savefirst < 2:
            nops = self.nops
        else:
            nops = self.nops // 10
        for i in range(0, nops):
            num = i + (self.mult * nops)
            if self.savefirst >= 2:
                key = self.savekey
            else:
                key = str(num) + self.bigkey + str(num)
            val = str(num) + self.bigval + str(num)
            c[key] = val
        if self.savefirst == 0:
            self.savekey = key
        self.savefirst += 1
        c.close()

        # Increase the multiplier so that later calls insert unique items.
        self.mult += 1
        # Increase the counter so that later backups have unique ids.
        if self.initial_backup == False:
            self.counter += 1

    def test_backup19(self):
        os.mkdir(self.bkp_home)
        self.home = self.bkp_home
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.setup_directories()

        self.pr('*** Add data, checkpoint, take backups and validate ***')
        self.pr('Adding initial data')
        self.initial_backup = True
        self.add_data(self.uri)
        self.take_full_backup()
        self.initial_backup = False
        self.session.checkpoint()

        self.add_data(self.uri)
        self.session.checkpoint()
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri)

if __name__ == '__main__':
    wttest.run()
