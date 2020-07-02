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

# test_backup16.py
# Ensure incremental backup doesn't copy unnecessary files.
class test_backup16(wttest.WiredTigerTestCase, suite_subprocess):

    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    counter=1
    logmax='100K'
    mult=1
    nops=10
    uri1='table:test1'
    uri2='table:test2'
    uri3='table:test3'
    uri4='table:test4'
    uri5='table:test5'

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 10
    bigval = 'Value' * 10

    def add_data(self, uri):

        c = self.session.open_cursor(uri)
        for i in range(0, self.nops):
            num = i + (self.mult * self.nops)
            key = self.bigkey + str(num)
            val = self.bigval + str(num)
            c[key] = val
        self.session.checkpoint()
        c.close()
        # Increase the multiplier so that later calls insert unique items.
        self.mult += 1

    def verify_incr_backup(self, expected_file_list):

        bkup_config = ('incremental=(src_id="ID' +  str(self.counter-1) +
                       '",this_id="ID' + str(self.counter) + '")')
        bkup_cur = self.session.open_cursor('backup:', None, bkup_config)
        self.counter += 1
        num_files = 0

        # Verify the files included in the incremental backup are the ones we expect.
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
                if ret != 0:
                    break

                # Check this file is one we are expecting.
                self.assertTrue(bkup_file in expected_file_list)
                num_files += 1

                # Ensure that the file we changed has content to copy and the file we didn't
                # change doesn't have any content to copy.
                # Note:
                # Pretty sure this is the file size and not the number of bytes to copy.
                incr_list = incr_cur.get_keys()
                self.assertNotEqual(incr_list[1], 0)

            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            incr_cur.close()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        bkup_cur.close()

        # Ensure that the backup saw all the expected files.
        self.assertEqual(num_files, len(expected_file_list))

    def test_backup16(self):

        self.session.create(self.uri1, 'key_format=S,value_format=S')
        self.session.create(self.uri2, 'key_format=S,value_format=S')
        self.session.create(self.uri3, 'key_format=S,value_format=S')
        self.add_data(self.uri1)
        self.add_data(self.uri2)

        # Checkpoint and simulate full backup.
        self.session.checkpoint()
        config = 'incremental=(enabled,granularity=1M,this_id="ID0")'
        cursor = self.session.open_cursor('backup:', None, config)
        cursor.close()

        # Create new tables, add more data and checkpoint.
        self.session.create(self.uri4, "key_format=S,value_format=S")
        self.session.create(self.uri5, "key_format=S,value_format=S")
        self.add_data(self.uri1)
        self.add_data(self.uri5)
        self.session.checkpoint()

        # Validate these three files are included in the incremental.
        files_to_backup = ['test1.wt', 'test4.wt', 'test5.wt']
        self.verify_incr_backup(files_to_backup)

        # Add more data and checkpoint.
        self.add_data(self.uri3)
        self.add_data(self.uri5)
        self.session.checkpoint()

        # Validate these three files are included in the incremental.
        files_to_backup = ['test3.wt', 'test4.wt', 'test5.wt']
        self.verify_incr_backup(files_to_backup)

        # Perform one more incremental without changing anything. We should only
        # see one file.
        files_to_backup = ['test4.wt']
        self.verify_incr_backup(files_to_backup)

if __name__ == '__main__':
    wttest.run()
