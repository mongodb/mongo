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
import os, glob, shutil
import wttest, wiredtiger
from suite_subprocess import suite_subprocess
from helper import compare_files

# Shared base class used by backup tests.
class backup_base(wttest.WiredTigerTestCase, suite_subprocess):
    data_cursor_config = None   # a config string for cursors.
    mult = 0                    # counter to have variance in data.
    nops = 100                  # number of operations added to uri.

    # We use counter to produce unique backup ids for multiple iterations
    # of incremental backup.
    bkup_id = 0
    # Setup some of the backup tests, and increments the backup id.
    initial_backup = False
    # Used for populate function.
    rows = 100
    populate_big = None

    # Specify a logpath directory to be used to place wiredtiger log files.
    logpath=''
    # Temporary directory used to verify consistent data between multiple incremental backups.
    home_tmp = "WT_TEST_TMP"

    #
    # Add data to the given uri.
    # Allows the option for doing a session checkpoint after adding data.
    #
    def add_data(self, uri, key, val, do_checkpoint=False):
        assert(self.nops != 0)
        c = self.session.open_cursor(uri, None, self.data_cursor_config)
        for i in range(0, self.nops):
            num = i + (self.mult * self.nops)
            k = key + str(num)
            v = val + str(num)
            c[k] = v
        c.close()
        if do_checkpoint:
            self.session.checkpoint()
        # Increase the counter so that later backups have unique ids.
        if not self.initial_backup:
            self.bkup_id += 1
        # Increase the multiplier so that later calls insert unique items.
        self.mult += 1

    #
    # Populate a set of objects.
    #
    def populate(self, objs, do_checkpoint=False, skiplsm=False):
        cg_config = ''
        for i in objs:
            if len(i) > 2:
                if i[2] and skiplsm:
                    continue
                if i[2] == self.populate_big:
                    self.rows = 50000 # Big Object
                else:
                    self.rows = 1000  # Small Object
            if len(i) > 3:
                cg_config = i[3]
            i[1](self, i[0], self.rows, cgconfig = cg_config).populate()

        # Backup needs a checkpoint.
        if do_checkpoint:
            self.session.checkpoint()

    #
    # Set up all the directories needed for the test. We have a full backup directory, an incremental backup and
    # temporary directory. The temp directory is used to hold updated data for incremental backups, and will overwrite
    # the contents of the incremental directory when this function is called, to setup future backup calls.
    # That way we can compare the full and incremental backup each time through.
    #
    # Note: The log directory is a subdirectory of the home directory, creating that will make the home directory also.
    # The incremental backup function, copies the latest data into the temporary directory.
    def setup_directories(self, home_incr, home_full):
        # Create the temp directory, if the path doesn't exist
        # as we only want to create this directory at the start
        if not os.path.exists(self.home_tmp):
            os.makedirs(self.home_tmp + '/' + self.logpath)

        if os.path.exists(home_full):
            shutil.rmtree(home_full)
        os.makedirs(home_full + '/' + self.logpath)

        # If the incremental directory exists, then remove the contents of the directory
        # and place all the contents of temporary directory into the incremental directory
        # such that the test can now perform further incremental backups on the directory.
        if os.path.exists(home_incr):
            shutil.rmtree(home_incr)
            shutil.copytree(self.home_tmp, self.home_incr)
        else:
            os.makedirs(home_incr + '/' + self.logpath)

    #
    # Check that a URI doesn't exist, both the meta-data and the file names.
    #
    def confirmPathDoesNotExist(self, uri, dir):
        conn = self.wiredtiger_open(dir)
        session = conn.open_session()
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session.open_cursor(uri, None, None))
        conn.close()

        self.assertEqual(
            glob.glob(dir + '*' + uri.split(":")[1] + '*'), [],
            'confirmPathDoesNotExist: URI exists, file name matching \"' +
            uri.split(":")[1] + '\" found')

    #
    # Copy a file into given directory.
    #
    def copy_file(self, file, dir):
        copy_from = file
        # If it is log file, prepend the path.
        if self.logpath and "WiredTigerLog" in file:
            copy_to = dir + '/' + self.logpath
        else:
            copy_to = dir
        shutil.copy(copy_from, copy_to)

    #
    # Uses a backup cursor to perform a selective backup, by iterating through the cursor
    # grabbing files that do not exist in the remove list to copy over into a given directory. 
    # When dealing with a test that performs multiple incremental backups, we need to perform a 
    # proper backup on each incremental directory as a starting base.
    #
    def take_selective_backup(self, backup_dir, remove_list, backup_cur=None):
        self.pr('Selective backup to ' + backup_dir + ': ')
        bkup_c = backup_cur
        if backup_cur == None:
            config = None
            if self.initial_backup:
                config = 'incremental=(granularity=1M,enabled=true,this_id=ID0)'
            bkup_c = self.session.open_cursor('backup:', None, config)
        all_files = []

        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while bkup_c.next() == 0:
            newfile = bkup_c.get_key()
            sz = os.path.getsize(newfile)
            if (newfile in remove_list):
                continue
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            self.copy_file(newfile, backup_dir)
            all_files.append(newfile)

        if backup_cur == None:
            bkup_c.close()
        return all_files

    #
    # Uses a backup cursor to perform a full backup, by iterating through the cursor
    # grabbing files to copy over into a given directory. When dealing with a test
    # that performs multiple incremental backups, we initially perform a full backup
    # on each incremental directory as a starting base.
    # Optional arguments:
    # backup_cur: A backup cursor that can be given into the function, but function caller
    #    holds reponsibility of closing the cursor.
    #
    def take_full_backup(self, backup_dir, backup_cur=None):
        self.pr('Full backup to ' + backup_dir + ': ')
        bkup_c = backup_cur
        if backup_cur == None:
            config = None
            if self.initial_backup:
                config = 'incremental=(granularity=1M,enabled=true,this_id=ID0)'
            bkup_c = self.session.open_cursor('backup:', None, config)
        all_files = []
        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while bkup_c.next() == 0:
            newfile = bkup_c.get_key()
            sz = os.path.getsize(newfile)
            self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + self.dir)
            self.copy_file(newfile, backup_dir)
            all_files.append(newfile)
        if backup_cur == None:
            bkup_c.close()
        return all_files

    #
    # Compare against two directory paths using the wt dump command.
    # The suffix allows the option to add distinctive tests adding suffix to the output files.
    #
    def compare_backups(self, uri, base_dir, other_dir, suffix = None):
        sfx = ""
        if suffix != None:
            sfx = "." + suffix
        base_out = "./backup_base" + sfx
        if os.path.exists(base_out):
            os.remove(base_out)

        # Run wt dump on base backup directory
        self.runWt(['-R', '-h', base_dir, 'dump', uri], outfilename=base_out)
        other_out = "./backup_other" + sfx
        if os.path.exists(other_out):
            os.remove(other_out)
        # Run wt dump on incremental backup
        self.runWt(['-R', '-h', other_dir, 'dump', uri], outfilename=other_out)
        self.pr("compare_files: " + base_out + ", " + other_out)
        self.assertEqual(True, compare_files(self, base_out, other_out))

    #
    # Perform a block range copy for a given offset and file.
    #
    def range_copy(self, filename, offset, size, backup_incr_dir, consolidate):
        read_from = filename
        write_to = backup_incr_dir  + '/' + filename
        rfp = open(read_from, "rb")
        rfp.seek(offset, 0)
        buf = rfp.read(size)
        # Perform between previous incremental directory, to check that
        # the old file and the new file is different. We can only ensure
        # the data are different if running in consolidate mode. It's
        # possible that we change multiple blocks in a single write and
        # some of the blocks are the same as before. If we are not running
        # in consolidate mode, these blocks which are copied separately one
        # by one will trigger this assert.
        old_to = self.home_tmp + '/' + filename
        if os.path.exists(old_to) and consolidate:
            self.pr('RANGE CHECK file ' + old_to + ' offset ' + str(offset) + ' len ' + str(size))
            old_rfp = open(old_to, "rb")
            old_rfp.seek(offset, 0)
            old_buf = old_rfp.read(size)
            old_rfp.close()
            # This assertion tests that the offset range we're given actually changed
            # from the previous backup.
            self.assertNotEqual(buf, old_buf)
        wfp = None
        # Create file if the file doesn't exist.
        if not os.path.exists(write_to):
            wfp = open(write_to, "w+b")
        else:
            wfp = open(write_to, "r+b")
        wfp.seek(offset, 0)
        wfp.write(buf)
        rfp.close()
        wfp.close()

    #
    # With a given backup cursor, open an incremental block cursor to copy the blocks of a
    # given file. If the type of file is WT_BACKUP_FILE, perform full copy into given directory,
    # otherwise if type of file is WT_BACKUP_RANGE, perform partial copy of the file using range copy.
    #
    # Note: we return the sizes of WT_BACKUP_RANGE type files for tests that check for consolidate config.
    #
    def take_incr_backup_block(self, bkup_c, newfile, backup_incr_dir, consolidate):
        config = 'incremental=(file=' + newfile + ')'
        self.pr('Open incremental cursor with ' + config)
        # For each file listed, open a duplicate backup cursor and copy the blocks.
        incr_c = self.session.open_cursor(None, bkup_c, config)
        # For consolidate
        lens = []
        # We cannot use 'for newfile in incr_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while incr_c.next() == 0:
            incrlist = incr_c.get_keys()
            offset = incrlist[0]
            size = incrlist[1]
            curtype = incrlist[2]
            self.assertTrue(curtype == wiredtiger.WT_BACKUP_FILE or curtype == wiredtiger.WT_BACKUP_RANGE)
            if curtype == wiredtiger.WT_BACKUP_FILE:
                sz = os.path.getsize(newfile)
                self.pr('Copy from: ' + newfile + ' (' + str(sz) + ') to ' + backup_incr_dir)
                # Copy the whole file.
                self.copy_file(newfile, backup_incr_dir)
            else:
                # Copy the block range.
                self.pr('Range copy file ' + newfile + ' offset ' + str(offset) + ' len ' + str(size))
                self.range_copy(newfile, offset, size, backup_incr_dir, consolidate)
                lens.append(size)
        incr_c.close()
        return lens

    #
    # Given a backup cursor, open a log cursor, and copy all log files that are not
    # in the given log list. Return all the log files.
    #
    def take_log_backup(self, bkup_c, backup_dir, orig_logs, log_cursor=None):
        # Now open a duplicate backup cursor.
        dupc = log_cursor
        if log_cursor == None:
            config = 'target=("log:")'
            dupc = self.session.open_cursor(None, bkup_c, config)
        dup_logs = []
        while dupc.next() == 0:
            newfile = dupc.get_key()
            self.assertTrue("WiredTigerLog" in newfile)
            sz = os.path.getsize(newfile)
            if (newfile not in orig_logs):
                self.pr('DUP: Copy from: ' + newfile + ' (' + str(sz) + ') to ' + backup_dir)
                shutil.copy(newfile, backup_dir)
            # Record all log files returned for later verification.
            dup_logs.append(newfile)
        if log_cursor == None:
            dupc.close()
        return dup_logs

    #
    # Open incremental backup cursor, with an id and iterate through all the files
    # and perform incremental block copy for each of them. Returns the information about
    # the backup files.
    #
    # Optional arguments:
    #   consolidate: Add consolidate option to the cursor.
    #
    def take_incr_backup(self, backup_incr_dir, id=0, consolidate=False):
        self.assertTrue(id > 0 or self.bkup_id > 0)
        if id == 0:
            id = self.bkup_id
        # Open the backup data source for incremental backup.
        config = 'incremental=(src_id="ID' +  str(id - 1) + '",this_id="ID' + str(id) + '"'
        if consolidate:
            config += ',consolidate=true'
        config += ')'
        self.pr("Incremental backup cursor with config " + config)
        bkup_c = self.session.open_cursor('backup:', None, config)

        file_sizes = []
        file_names = []

        # We cannot use 'for newfile in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        # If that changes then this, and the use of the duplicate below can change.
        while bkup_c.next() == 0:
            newfile = bkup_c.get_key()
            file_sizes += self.take_incr_backup_block(bkup_c, newfile, backup_incr_dir, consolidate)
            file_names.append(newfile)
            # Copy into temp directory for tests that require further iterations of incremental backups.
            self.copy_file(newfile, self.home_tmp)
        bkup_c.close()
        return (file_names, file_sizes)
