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

import os, re, time
from wtscenario import make_scenarios
from wtbackup import backup_base
from wiredtiger import stat

# test_backup29.py
#    Test interaction between checkpoint and incremental backup. There was a bug in
# maintaining the incremental backup bitmaps correctly after opening an uncached dhandle.
# This test reconstructs the failure scenario and verifies correct behavior both when a
# restart and when dhandle sweep lead to opening an uncached dhandle.
class test_backup29(backup_base):
    conn_config = 'file_manager=(close_handle_minimum=0,' + \
              'close_idle_time=3,close_scan_interval=1),' + \
              'statistics=(fast)'
    create_config = 'allocation_size=512,key_format=i,value_format=S'
    # Backup directory name. Uncomment if actually taking a backup.
    # dir='backup.dir'
    uri1 = 'test_first'
    uri2 = 'test_second'
    file1_uri = 'file:' + uri1 + '.wt'
    file2_uri = 'file:' + uri2 + '.wt'
    table1_uri = 'table:' + uri1
    table2_uri = 'table:' + uri2
    active_uri = 'table:active.wt'

    value_base = '-abcdefghijkl'

    few = 100
    nentries = 5000

    def get_stat(self, stat_name):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_name][2]
        stat_cursor.close()
        return value

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

    def compare_bitmap(self, orig, new):
        # Compare the bitmaps from the metadata. Once a bit is set, it should never
        # be cleared. But new bits could be set. So the check is only: if the original
        # bitmap has a bit set then the current bitmap must be set for that bit also.
        #
        # First convert both bitmaps to a binary string, accounting for any possible leading
        # zeroes (that would be truncated off). Then compare bit by bit.
        orig_bits = bin(int('1'+orig, 16))[3:]
        new_bits = bin(int('1'+new, 16))[3:]
        self.pr("Original bitmap in binary: " + orig_bits)
        self.pr("Reopened bitmap in binary: " + new_bits)
        for o_bit, n_bit in zip(orig_bits, new_bits):
            if o_bit != '0':
                self.assertTrue(n_bit != '0')

    def setup_test(self):
        # Create and populate the tables.
        self.session.create(self.table1_uri, self.create_config)
        self.session.create(self.table2_uri, self.create_config)
        c1 = self.session.open_cursor(self.table1_uri)
        c2 = self.session.open_cursor(self.table2_uri)
        # Only add a few entries.
        self.pr("Write: " + str(self.few) + " initial data items")
        for i in range(1, self.few):
            val = str(i) + self.value_base
            c1[i] = val
            c2[i] = val
        self.session.checkpoint()

        # Take the initial full backup for incremental. We don't actually need to
        # take the backup, we only need to open and close the backup cursor to have
        # the library keep track of the bitmaps.
        config = 'incremental=(enabled,granularity=4k,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        self.assertEqual(1, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(1, self.get_stat(stat.conn.backup_incremental))
        # Uncomment these lines if actually taking the full backup is helpful for debugging.
        # os.mkdir(self.dir)
        # self.take_full_backup(self.dir, bkup_c)
        bkup_c.close()

        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(1, self.get_stat(stat.conn.backup_incremental))

        # Add a lot more data to both tables to generate a filled-in block mod bitmap.
        last_i = self.few
        self.pr("Write: " + str(self.nentries) + " additional data items")
        for i in range(self.few, self.nentries):
            val = str(i) + self.value_base
            c1[i] = val
            c2[i] = val
        c1.close()
        c2.close()
        self.session.checkpoint()
        # Get the block mod bitmap from the file URI.
        self.orig1_bitmap = self.parse_blkmods(self.file1_uri)
        self.orig2_bitmap = self.parse_blkmods(self.file2_uri)


    def incr_backup_and_validate(self):
        # After reopening we want to open both tables, but only modify one of them for
        # the first checkpoint. Then modify the other table, checkpoint, and then check the
        # that the block mod bitmap remains correct for the other table.
        c1 = self.session.open_cursor(self.table1_uri)
        c2 = self.session.open_cursor(self.table2_uri)
        last_i = self.nentries

        # Change the first table and checkpoint. Keep the second table clean.
        self.pr("Update only table 1: " + str(last_i))
        val = str(last_i) + self.value_base
        c1[last_i] = val
        self.session.checkpoint()
        new1_bitmap = self.parse_blkmods(self.file1_uri)

        # Now change the second table and checkpoint again.
        self.pr("Update second table: " + str(last_i))
        c2[last_i] = val
        self.session.checkpoint()
        new2_bitmap = self.parse_blkmods(self.file2_uri)

        c1.close()
        c2.close()

        self.compare_bitmap(self.orig1_bitmap, new1_bitmap)
        self.compare_bitmap(self.orig2_bitmap, new2_bitmap)

    def test_backup29_reopen(self):
        self.setup_test()
        # Make sure all the stats are not set
        self.assertEqual(0, self.get_stat(stat.conn.backup_blocks))
        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(0, self.get_stat(stat.conn.backup_dup_open))
        self.assertEqual(0, self.get_stat(stat.conn.backup_start))

        self.pr("CLOSE and REOPEN conn")
        self.reopen_conn()
        self.assertEqual(0, self.get_stat(stat.conn.backup_blocks))
        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(0, self.get_stat(stat.conn.backup_dup_open))
        # NOTE: Incremental should be set after restart.
        self.assertEqual(1, self.get_stat(stat.conn.backup_incremental))
        self.assertEqual(0, self.get_stat(stat.conn.backup_start))
        self.pr("Reopened conn")

        self.incr_backup_and_validate()

    def test_backup29_sweep(self):
        self.setup_test()

        self.pr("Waiting to sweep handles")
        # Create another table and populate it, and checkpoint.
        self.session.create(self.active_uri, self.create_config)
        c = self.session.open_cursor(self.active_uri)
        for i in range(1, self.few):
            c[i] = str(i) + self.value_base
        self.session.checkpoint()

        sleep = 0
        max = 20
        # The only files sweep won't close should be the metadata, the history store, the
        # lock file, the statistics file, and our active file.
        final_nfile = 5

        # Keep updating and checkpointing this table until all other handles have been swept.
        # The checkpoints have the side effect of sweeping the session cache, which will allow
        # dhandles to be closed.
        while sleep < max:
            i = i + 1
            c[i] = str(i) + self.value_base
            self.session.checkpoint()
            sleep += 0.5
            time.sleep(0.5)
            nfile = self.get_stat(stat.conn.file_open)
            if nfile == final_nfile:
                break
        c.close()

        # Make sure we swept everything before we ran out of time.
        self.assertEqual(nfile, final_nfile)
        self.pr("Sweep done")

        self.incr_backup_and_validate()
