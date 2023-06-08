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

import os, re
from wtscenario import make_scenarios
from wtbackup import backup_base

# test_backup29.py
#    Test interaction between restart, checkpoint and incremental backup. There was a bug in
# maintaining the incremental backup bitmaps correctly across restarts in specific conditions
# that this test can reproduce.
#
class test_backup29(backup_base):
    create_config = 'allocation_size=512,key_format=i,value_format=S'
    # Backup directory name. Uncomment if actually taking a backup.
    # dir='backup.dir'
    uri = 'test_backup29'
    uri2 = 'test_other'
    value_base = '-abcdefghijkl'

    few = 100
    nentries = 5000

    def parse_blkmods(self, uri):
        meta_cursor = self.session.open_cursor('metadata:')
        config = meta_cursor[uri]
        meta_cursor.close()
        # The search string looks like: ,blocks=feffff1f000000000000000000000000
        # Obtain just the hex string.
        b = re.search(',blocks=(\w+)', config)
        self.assertTrue(b is not None)
        # The bitmap string after the = is in group 1.
        blocks = b.group(1)
        self.pr("block bitmap: " + blocks)
        return blocks

    def test_backup29(self):

        # Create and populate the table.
        file_uri = 'file:' + self.uri + '.wt'
        file2_uri = 'file:' + self.uri2 + '.wt'
        table_uri = 'table:' + self.uri
        table2_uri = 'table:' + self.uri2
        self.session.create(table_uri, self.create_config)
        self.session.create(table2_uri, self.create_config)
        c = self.session.open_cursor(table_uri)
        c2 = self.session.open_cursor(table2_uri)
        # Only add a few entries.
        self.pr("Write: " + str(self.few) + " initial data items")
        for i in range(1, self.few):
            val = str(i) + self.value_base
            c[i] = val
            c2[i] = val
        self.session.checkpoint()

        # Take the initial full backup for incremental. We don't actually need to
        # take the backup, we only need to open and close the backup cursor to have
        # the library keep track of the bitmaps.
        config = 'incremental=(enabled,granularity=4k,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        # Uncomment these lines if actually taking the full backup is helpful for debugging.
        # os.mkdir(self.dir)
        # self.take_full_backup(self.dir, bkup_c)
        bkup_c.close()

        # Add a lot more data to both tables to generate a filled-in block mod bitmap.
        last_i = self.few
        self.pr("Write: " + str(self.nentries) + " additional data items")
        for i in range(self.few, self.nentries):
            val = str(i) + self.value_base
            c[i] = val
            c2[i] = val
        last_i = self.nentries
        c.close()
        c2.close()
        self.session.checkpoint()
        # Get the block mod bitmap from the file URI.
        orig_bitmap = self.parse_blkmods(file2_uri)
        self.pr("CLOSE and REOPEN conn")
        self.reopen_conn()
        self.pr("Reopened conn")

        # After reopening we want to open both tables, but only modify one of them for
        # the first checkpoint. Then modify the other table, checkpoint, and then check the
        # that the block mod bitmap remains correct for the other table.
        c = self.session.open_cursor(table_uri)
        c2 = self.session.open_cursor(table2_uri)

        # Change one table and checkpoint. Keep the other table clean.
        self.pr("Update only table 1: " + str(last_i))
        val = str(last_i) + self.value_base
        c[last_i] = val
        self.session.checkpoint()

        # Now change the other table and checkpoint again.
        self.pr("Update second table: " + str(last_i))
        c2[last_i] = val
        self.session.checkpoint()
        new_bitmap = self.parse_blkmods(file2_uri)

        c.close()
        c2.close()

        # Compare the bitmaps from the metadata. Once a bit is set, it should never
        # be cleared. But new bits could be set. So the check is only: if the original
        # bitmap has a bit set then the current bitmap must be set for that bit also. 
        #
        # First convert both bitmaps to a binary string, accounting for any possible leading
        # zeroes (that would be truncated off). Then compare bit by bit.
        orig_bits = bin(int('1'+orig_bitmap, 16))[3:]
        new_bits = bin(int('1'+new_bitmap, 16))[3:]
        self.pr("Original bitmap in binary: " + orig_bits)
        self.pr("Reopened bitmap in binary: " + new_bits)
        for orig, new in zip(orig_bits, new_bits):
            if orig != '0':
                self.assertTrue(new != '0')

if __name__ == '__main__':
    wttest.run()
