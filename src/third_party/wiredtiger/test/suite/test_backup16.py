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

import wiredtiger, wttest
from wtbackup import backup_base

# test_backup16.py
# Ensure incremental backup doesn't copy unnecessary files.
class test_backup16(backup_base):

    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    logmax='100K'

    # Define the table name and its on-disk file name together.
    file1='test1.wt'
    uri1='table:test1'
    file2='test2.wt'
    uri2='table:test2'
    file3='test3.wt'
    uri3='table:test3'
    file4='test4.wt'
    uri4='table:test4'
    file5='test5.wt'
    uri5='table:test5'
    file6='test6.wt'
    uri6='table:test6'

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 10
    bigval = 'Value' * 10

    mult = 1
    bkup_id = 1
    nops = 10
    initial_backup = True
    def verify_incr_backup(self, expected_file_list):
        bkup_config = ('incremental=(src_id="ID' +  str(self.bkup_id - 1) +
                       '",this_id="ID' + str(self.bkup_id) + '")')
        bkup_cur = self.session.open_cursor('backup:', None, bkup_config)
        self.bkup_id += 1
        num_files = 0

        # Verify the files included in the incremental backup are the ones we expect.
        # Note that all files will be returned. We're only interested in the ones that
        # return file information.
        while True:
            ret = bkup_cur.next()
            if ret != 0:
                break

            bkup_file = bkup_cur.get_key()
            if bkup_file.startswith('WiredTiger'):
                continue

            incr_config = 'incremental=(file=' + bkup_file + ')'
            incr_cur = self.session.open_cursor(None, bkup_cur, incr_config)

            while True:
                ret = incr_cur.next()
                # Stop if there is nothing to copy or we're done.
                if ret != 0:
                    break

                # Check this file is one we are expecting.
                self.assertTrue(bkup_file in expected_file_list)
                num_files += 1

                # Ensure that the file we changed has content to copy and the file we didn't
                # change doesn't have any content to copy. The second value is the number of
                # bytes to copy for a range return or the file size on a file copy. Either way
                # it should not be zero.
                incr_list = incr_cur.get_keys()
                self.assertNotEqual(incr_list[1], 0)

            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            incr_cur.close()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_cur.close()

        # Ensure that the backup saw all the expected files.
        self.assertEqual(num_files, len(expected_file_list))

    def test_backup16(self):

        # Create four tables before the first backup. Add data to two of them.
        self.session.create(self.uri1, 'key_format=S,value_format=S')
        self.session.create(self.uri2, 'key_format=S,value_format=S')
        self.session.create(self.uri3, 'key_format=S,value_format=S')
        self.session.create(self.uri6, 'key_format=S,value_format=S')
        self.add_data(self.uri1, self.bigkey, self.bigval, True)
        self.add_data(self.uri2, self.bigkey, self.bigval, True)

        # Checkpoint and simulate full backup.
        self.session.checkpoint()
        config = 'incremental=(enabled,granularity=1M,this_id="ID0")'
        cursor = self.session.open_cursor('backup:', None, config)
        cursor.close()

        # Create new tables, add more data and checkpoint.
        # Add data to an earlier table and one of the new tables. This now gives us:
        # 1. An existing table with checkpointed new data and changes to record.
        # 2. An existing table with old data and no new changes.
        # 3. An existing table with no data at all but gets new data.
        # 4. An new table with no data and that will never have new data.
        # 5. A new table with checkpointed new data.
        # 6. An existing table with no data and that will never have new data.
        #
        # Note that cases 4 and 6 are different. In the case of table 6, since it existed on
        # disk at the time of the initial full backup it will never appear in the list of files
        # with any data to copy. In the case of table 4 we will be told to copy the whole file
        # every time in order to have the new file appear in the backup.
        #
        self.session.create(self.uri4, "key_format=S,value_format=S")
        self.session.create(self.uri5, "key_format=S,value_format=S")
        self.add_data(self.uri1, self.bigkey, self.bigval, True)
        self.add_data(self.uri5, self.bigkey, self.bigval, True)
        self.session.checkpoint()

        # Validate these three files are included in the incremental.
        # Both new tables should appear in the incremental and the old table with
        # new data.
        files_to_backup = [self.file1, self.file4, self.file5]
        self.verify_incr_backup(files_to_backup)

        # Add more data and checkpoint. Earlier old tables without new data should not
        # appear in the list. The table with no data at all continues to appear in the
        # list.
        self.add_data(self.uri3, self.bigkey, self.bigval, True)
        self.add_data(self.uri5, self.bigkey, self.bigval, True)

        self.session.checkpoint()
        # Validate these three files are included in the incremental.
        files_to_backup = [self.file3, self.file4, self.file5]
        self.verify_incr_backup(files_to_backup)

        # Perform one more incremental without changing anything. We should only
        # see one file, the file that does not have any checkpoint information.
        files_to_backup = [self.file4]
        self.verify_incr_backup(files_to_backup)

if __name__ == '__main__':
    wttest.run()
