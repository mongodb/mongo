#!/usr/bin/env python
#
# Public Domain 2014-2021 MongoDB, Inc.
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
import os, glob
import wttest, wiredtiger
from suite_subprocess import suite_subprocess
from helper import compare_files

# Shared base class used by backup tests.
class backup_base(wttest.WiredTigerTestCase, suite_subprocess):
    cursor_config = None        # a config string for cursors
    mult = 0                    # counter to have variance in data
    nops = 100                  # number of operations added to uri

    # We use counter to produce unique backup names for multiple iterations
    # of incremental backup tests.
    counter = 0
    # To determine whether to increase/decrease counter, which determines
    initial_backup = True
    # Used for populate function
    rows = 100
    populate_big = None

    #
    # Add data to the given uri.
    # Allows the option for doing a session checkpoint after adding data.
    #
    def add_data(self, uri, key, val, do_checkpoint=False):
        assert(self.nops != 0)
        c = self.session.open_cursor(uri, None, self.cursor_config)
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
            self.counter += 1
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

        # Backup needs a checkpoint
        if do_checkpoint:
            self.session.checkpoint()

    #
    # Set up all the directories needed for the test. We have a full backup directory for each
    # iteration and an incremental backup for each iteration. That way we can compare the full and
    # incremental each time through.
    #
    def setup_directories(self, max_iteration, home_incr, home_full, logpath):
        for i in range(0, max_iteration):
            # The log directory is a subdirectory of the home directory,
            # creating that will make the home directory also.

            home_incr_dir = home_incr + '.' + str(i)
            if os.path.exists(home_incr_dir):
                os.remove(home_incr_dir)
            os.makedirs(home_incr_dir + '/' + logpath)

            if i == 0:
                continue
            home_full_dir = home_full + '.' + str(i)
            if os.path.exists(home_full_dir):
                os.remove(home_full_dir)
            os.makedirs(home_full_dir + '/' + logpath)

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
    # Compare against two directory paths using the wt dump command.
    # The suffix allows the option to add distinctive tests adding suffix to both the output files and directories
    #
    def compare_backups(self, uri, base_dir_home, other_dir_home, suffix = None):
        sfx = ""
        if suffix != None:
            sfx = "." + suffix
        base_out = "./backup_base" + sfx
        base_dir = base_dir_home + sfx

        if os.path.exists(base_out):
            os.remove(base_out)

        # Run wt dump on base backup directory
        self.runWt(['-R', '-h', base_dir, 'dump', uri], outfilename=base_out)
        other_out = "./backup_other" + sfx
        if os.path.exists(other_out):
            os.remove(other_out)
        # Run wt dump on incremental backup
        other_dir = other_dir_home + sfx
        self.runWt(['-R', '-h', other_dir, 'dump', uri], outfilename=other_out)
        self.assertEqual(True, compare_files(self, base_out, other_out))
