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

# test_backup14.py
# Test cursor backup with a block-based incremental cursor.
class test_backup14(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:main"
    uri2="table:extra"
    uri_logged="table:logged_table"
    uri_not_logged="table:not_logged_table"
    full_out = "./backup_block_full"
    incr_out = "./backup_block_incr"
    bkp_home = "WT_BLOCK"
    home_full = "WT_BLOCK_LOG_FULL"
    home_incr = "WT_BLOCK_LOG_INCR"
    logpath = "logpath"
    nops=1000
    mult=0
    max_iteration=7
    counter=0
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
        for i in range(0, self.max_iteration):
            remove_dir = self.home_incr + '.' + str(i)

            create_dir = self.home_incr + '.' + str(i) + '/' + self.logpath
            if os.path.exists(remove_dir):
                os.remove(remove_dir)
            os.makedirs(create_dir)

            if i == 0:
                continue
            remove_dir = self.home_full + '.' + str(i)
            create_dir = self.home_full + '.' + str(i) + '/' + self.logpath
            if os.path.exists(remove_dir):
                os.remove(remove_dir)
            os.makedirs(create_dir)

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

        cursor = self.session.open_cursor('backup:', None, buf)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            newfile = cursor.get_key()

            if self.counter == 0:
                # Take a full bakcup into each incremental directory
                for i in range(0, self.max_iteration):
                    copy_from = newfile
                    # If it is log file, prepend the path.
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
        cursor.close()

    def take_incr_backup(self):
        # Open the backup data source for incremental backup.
        buf = 'incremental=(src_id="ID' +  str(self.counter-1) + '",this_id="ID' + str(self.counter) + '")'
        bkup_c = self.session.open_cursor('backup:', None, buf)
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
            incr_c = self.session.open_cursor(None, bkup_c, config)

            # For each file listed, open a duplicate backup cursor and copy the blocks.
            while True:
                ret = incr_c.next()
                if ret != 0:
                    break
                incrlist = incr_c.get_keys()
                offset = incrlist[0]
                size = incrlist[1]
                curtype = incrlist[2]
                # 1 is WT_BACKUP_FILE
                # 2 is WT_BACKUP_RANGE
                self.assertTrue(curtype == 1 or curtype == 2)
                if curtype == 1:
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
                    self.pr('Range copy file ' + newfile + ' offset ' + str(offset) + ' len ' + str(size))
                    write_from = newfile
                    write_to = self.home_incr + '.' + str(self.counter) + '/' + newfile
                    rfp = open(write_from, "r+b")
                    wfp = open(write_to, "w+b")
                    rfp.seek(offset, 0)
                    wfp.seek(offset, 0)
                    buf = rfp.read(size)
                    wfp.write(buf)
                    rfp.close()
                    wfp.close()
                dup_cnt += 1
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            incr_c.close()

            # For each file, we want to copy the file into each of the later incremental directories
            for i in range(self.counter, self.max_iteration):
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
        #
        # Run wt dump on full backup directory
        #
        full_backup_out = self.full_out + '.' + str(self.counter)
        home_dir = self.home_full + '.' + str(self.counter)
        if self.counter == 0:
            home_dir = self.home

        self.runWt(['-R', '-h', home_dir, 'dump', t_uri], outfilename=full_backup_out)
        #
        # Run wt dump on incremental backup directory
        #
        incr_backup_out = self.incr_out + '.' + str(self.counter)
        home_dir = self.home_incr + '.' + str(self.counter)
        self.runWt(['-R', '-h', home_dir, 'dump', t_uri], outfilename=incr_backup_out)

        self.assertEqual(True,
            compare_files(self, full_backup_out, incr_backup_out))

    #
    # Add data to the given uri.
    #
    def add_data(self, uri, bulk_option):
        c = self.session.open_cursor(uri, None, bulk_option)
        for i in range(0, self.nops):
            num = i + (self.mult * self.nops)
            key = self.bigkey + str(num)
            val = self.bigval + str(num)
            c[key] = val
        c.close()

        # Increase the multiplier so that later calls insert unique items.
        self.mult += 1
        # Increase the counter so that later backups have unique ids.
        if self.initial_backup == False:
            self.counter += 1

    #
    # Remove data from uri (table:main)
    #
    def remove_data(self):
        c = self.session.open_cursor(self.uri)
        #
        # We run the outer loop until mult value to make sure we remove all the inserted records
        # from the main table.
        #
        for i in range(0, self.mult):
            for j in range(i, self.nops):
                num = j + (i * self.nops)
                key = self.bigkey + str(num)
                c.set_key(key)
                self.assertEquals(c.remove(), 0)
        c.close()
        # Increase the counter so that later backups have unique ids.
        self.counter += 1

    #
    # This function will add records to the table (table:main), take incremental/full backups and
    # validate the backups.
    #
    def add_data_validate_backups(self):
        self.pr('Adding initial data')
        self.initial_backup = True
        self.add_data(self.uri, None)
        self.take_full_backup()
        self.initial_backup = False
        self.session.checkpoint()

        self.add_data(self.uri, None)
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri)

    #
    # This function will remove all the records from table (table:main), take backup and validate the
    # backup.
    #
    def remove_all_records_validate(self):
        self.remove_data()
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri)

    #
    # This function will drop the existing table uri (table:main) that is part of the backups and
    # create new table uri2 (table:extra), take incremental backup and validate.
    #
    def drop_old_add_new_table(self):

        # Drop main table.
        self.session.drop(self.uri)

        # Create uri2 (table:extra)
        self.session.create(self.uri2, "key_format=S,value_format=S")

        self.new_table = True
        self.add_data(self.uri2, None)
        self.take_incr_backup()

        table_list = 'tablelist.txt'
        # Assert if the dropped table (table:main) exists in the incremental folder.
        self.runWt(['-R', '-h', self.home, 'list'], outfilename=table_list)
        ret = os.system("grep " + self.uri + " " + table_list)
        self.assertNotEqual(ret, 0, self.uri + " dropped, but table exists in " + self.home)

    #
    # This function will create previously dropped table uri (table:main) and add different content to
    # it, take backups and validate the backups.
    #
    def create_dropped_table_add_new_content(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data(self.uri, None)
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri)

    #
    # This function will insert bulk data in logged and not-logged table, take backups and validate the
    # backups.
    #
    def insert_bulk_data(self):
        #
        # Insert bulk data into uri3 (table:logged_table).
        #
        self.session.create(self.uri_logged, "key_format=S,value_format=S")
        self.add_data(self.uri_logged, 'bulk')
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri_logged)

        #
        # Insert bulk data into uri4 (table:not_logged_table).
        #
        self.session.create(self.uri_not_logged, "key_format=S,value_format=S,log=(enabled=false)")
        self.add_data(self.uri_not_logged, 'bulk')
        self.take_full_backup()
        self.take_incr_backup()
        self.compare_backups(self.uri_not_logged)

    def test_backup14(self):
        os.mkdir(self.bkp_home)
        self.home = self.bkp_home
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.setup_directories()

        self.pr('*** Add data, checkpoint, take backups and validate ***')
        self.add_data_validate_backups()

        self.pr('*** Remove old records and validate ***')
        self.remove_all_records_validate()

        self.pr('*** Drop old and add new table ***')
        self.drop_old_add_new_table()

        self.pr('*** Create previously dropped table and add new content ***')
        self.create_dropped_table_add_new_content()

        self.pr('*** Insert data into Logged and Not-Logged tables ***')
        self.insert_bulk_data()

if __name__ == '__main__':
    wttest.run()
