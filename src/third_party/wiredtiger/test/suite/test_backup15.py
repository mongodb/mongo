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

# test_backup15.py
# Test cursor backup with a block-based incremental cursor.
class test_backup15(backup_base):
    bkp_home = "WT_BLOCK"
    bkup_id=0
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    logmax="100K"
    max_iteration=5
    mult=0
    nops=100000
    savefirst=0
    savekey='NOTSET'
    uri="table:main"

    dir='backup.dir'                    # Backup directory name
    home_full = "WT_BLOCK_LOG_FULL"
    home_incr = "WT_BLOCK_LOG_INCR"

    logpath = "logpath"
    new_table=False

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    #
    # Add data to the given uri.
    #
    def add_complex_data(self, uri):
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
            self.bkup_id += 1

    def test_backup15(self):
        os.mkdir(self.bkp_home)
        self.home = self.bkp_home
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.setup_directories(self.home_incr, self.home_full)

        self.pr('*** Add data, checkpoint, take backups and validate ***')
        self.pr('Adding initial data')
        self.initial_backup = True
        self.add_complex_data(self.uri)
        self.take_full_backup(self.home_incr)
        self.initial_backup = False
        self.session.checkpoint()
        # Each call now to take a full backup will make a copy into a full directory. Then
        # each incremental will take an incremental backup and we can compare them.
        for i in range(1, self.max_iteration):
            self.add_complex_data(self.uri)
            self.session.checkpoint()
            # Swap the order of the full and incremental backups. It should not matter. They
            # should not interfere with each other.
            if i % 2 == 0:
                self.take_full_backup(self.home_full)
                self.take_incr_backup(self.home_incr)
            else:
                self.take_incr_backup(self.home_incr)
                self.take_full_backup(self.home_full)
            self.compare_backups(self.uri, self.home_full, self.home_incr, str(self.bkup_id))
            self.setup_directories(self.home_incr, self.home_full)
