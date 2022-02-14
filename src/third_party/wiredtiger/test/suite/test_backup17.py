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

import wttest
import os
from wtbackup import backup_base

# test_backup17.py
# Test cursor backup with a block-based incremental cursor and consolidate.
class test_backup17(backup_base):
    dir='backup.dir'                    # Backup directory name
    gran="100K"
    granval=100*1024
    logmax="100K"
    uri="table:test"
    uri2="table:test2"

    conn_config='cache_size=1G,log=(enabled,file_max=%s)' % logmax

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    nops = 1000

    #
    # With a file length list, and the consolidate option is used, we expect the incremental
    # backup to collapse adjacent blocks and return us lengths that exceed the granularity setting
    # and verify that we see multiple blocks. If consolidate is not used, no block lengths should
    # ever be greater than the granularity setting.
    #
    def check_consolidate_sizes(self, file_lens, consolidate):
        saw_multiple = False
        for size in file_lens:
            if size > self.granval:
                saw_multiple = True
        if consolidate:
            self.assertTrue(saw_multiple)
        else:
            self.assertFalse(saw_multiple)

    def test_backup17(self):

        self.session.create(self.uri, "key_format=S,value_format=S")
        self.session.create(self.uri2, "key_format=S,value_format=S")
        self.add_data(self.uri, self.bigkey, self.bigval, True)
        self.mult = 0
        self.add_data(self.uri2, self.bigkey, self.bigval, True)

        os.mkdir(self.dir)
        # Open up the backup cursor. This causes a new log file to be created.
        # That log file is not part of the list returned. This is a full backup
        # primary cursor with incremental configured.
        config = 'incremental=(enabled,granularity=%s,this_id="ID1")' % self.gran
        bkup_c = self.session.open_cursor('backup:', None, config)

        # Now make a full backup and track the log files.
        self.take_full_backup(self.dir, bkup_c)
        bkup_c.close()

        # This is the main part of the test for consolidate. Add data to the first table.
        # Then perform the incremental backup with consolidate off (the default). Then add the
        # same data to the second table. Perform an incremental backup with consolidate on and
        # verify we get fewer, consolidated values.
        self.mult = 1
        self.add_data(self.uri, self.bigkey, self.bigval, True)

        # Do an incremental backup with id 2.
        (_, uri1_lens) = self.take_incr_backup(self.dir, 2, False)
        self.check_consolidate_sizes(uri1_lens, False)

        self.mult = 1
        self.add_data(self.uri2, self.bigkey, self.bigval, True)

        # Now do an incremental backup with id 3.
        (_, uri2_lens) = self.take_incr_backup(self.dir, 3, True)
        self.check_consolidate_sizes(uri2_lens, True)

        # Assert that we recorded fewer lengths on the consolidated backup.
        self.assertLess(len(uri2_lens), len(uri1_lens))
        # Assert that we recorded the same total data length for both.
        self.assertEqual(sum(uri2_lens), sum(uri1_lens))

if __name__ == '__main__':
    wttest.run()
