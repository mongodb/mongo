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

import os, shutil, threading, time
from wtthread import checkpoint_thread
from helper import simulate_crash_restart
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from helper import copy_wiredtiger_home
from wiredtiger import stat

# test_checkpoint_snapshot06.py
#   This test is to run checkpoint and truncate and insert followed by eviction
#   for one table in parallel with timing stress for checkpoint and let eviction
#   write more data than checkpoint.
class test_checkpoint_snapshot06(wttest.WiredTigerTestCase):

    # Create two tables.
    uri_1 = "table:test_checkpoint_snapshot06_1"
    uri_2 = "table:test_checkpoint_snapshot06_2"
    backup_dir = "BACKUP"

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    restart_values = [
        ("crash_restart", dict(restart=True)),
        ("backup", dict(restart=False)),
    ]

    scenarios = make_scenarios(format_values, restart_values)

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true),debug_mode=(log_retention=10),timing_stress_for_test=[checkpoint_slow]'
        return config

    def moresetup(self):
        if self.value_format == '8t':
            # Rig to use more than one page; otherwise the inconsistent checkpoint assertions fail.
            self.extraconfig = ',leaf_page_max=4096'
            self.nrows = 5000
            self.valuea = 97
            self.valueb = 98
        else:
            self.extraconfig = ''
            self.nrows = 1000
            self.valuea = "aaaaa" * 100
            self.valueb = "bbbbb" * 100

    def take_full_backup(self, fromdir, todir):
        # Open up the backup cursor, and copy the files.  Do a full backup.
        cursor = self.session.open_cursor('backup:', None, None)
        self.pr('Full backup from '+ fromdir + ' to ' + todir + ': ')
        os.mkdir(todir)
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            bkup_file = cursor.get_key()
            copy_file = os.path.join(fromdir, bkup_file)
            sz = os.path.getsize(copy_file)
            self.pr('Copy from: ' + bkup_file + ' (' + str(sz) + ') to ' + todir)
            shutil.copy(copy_file, todir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

    def evict_cursor(self, uri, ds, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (1, nrows + 1):
            evict_cursor.set_key(ds.key(i))
            evict_cursor.search()
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows+1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            if commit_ts == 0:
                session.commit_transaction()
            else:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts, more_invisible_rows_exist):
        # In FLCS the existence of the invisible extra set of rows causes the table to
        # extend under them. Until that's fixed, expect (not just allow) those rows to
        # exist and demand that they read back as zero and not as check_value. When it
        # is fixed (so the end of the table updates transactionally) the special-case
        # logic can just be removed.
        flcs_tolerance = more_invisible_rows_exist and self.value_format == '8t'

        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if flcs_tolerance and count >= nrows:
                self.assertEqual(v, 0)
            else:
                self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows * 2 if flcs_tolerance else nrows)

    def perform_backup_or_crash_restart(self, fromdir, todir):
        if self.restart == True:
            #Simulate a crash by copying to a new directory(RESTART).
            copy_wiredtiger_home(self, fromdir, todir + ".copy", True)
            simulate_crash_restart(self, fromdir, todir)
        else:
            #Take a backup and restore it.
            self.take_full_backup(fromdir, todir)
            self.take_full_backup(fromdir, todir + ".copy")
            self.reopen_conn(todir)

    def test_checkpoint_snapshot(self):
        self.moresetup()

        ds_1 = SimpleDataSet(self, self.uri_1, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config="log=(enabled=true)" +self.extraconfig)
        ds_1.populate()

        ds_2 = SimpleDataSet(self, self.uri_2, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config="log=(enabled=true)" + self.extraconfig)
        ds_2.populate()

        # Insert number of records into both tables.
        self.large_updates(self.uri_1, self.valuea, ds_1, self.nrows, 0)
        self.check(self.valuea, self.uri_1, self.nrows, 0, False)

        self.large_updates(self.uri_2, self.valuea, ds_2, self.nrows, 0)
        self.check(self.valuea, self.uri_2, self.nrows, 0, False)

        # Remove one key from both the tables.
        cursor1 = self.session.open_cursor(self.uri_1)
        cursor2 = self.session.open_cursor(self.uri_2)

        cursor1.set_key(ds_1.key(50))
        cursor2.set_key(ds_2.key(50))
        self.assertEqual(cursor1.remove(), 0)
        self.assertEqual(cursor2.remove(), 0)

        # Truncate the range from 1-100 in both tables where key 50 doesn't exist.
        # We only set a stop cursor for both tables and send in an empty start
        # cursor to truncate from the beginning of the table.
        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor11 = session1.open_cursor(self.uri_1)
        cursor12 = session1.open_cursor(self.uri_2)

        cursor11.set_key(ds_1.key(100))
        cursor12.set_key(ds_2.key(100))
        session1.truncate(None, None, cursor11, None)
        session1.truncate(None, None, cursor12, None)

        # Insert the key 50 that was already removed.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        cursor21 = session2.open_cursor(self.uri_1)
        cursor22 = session2.open_cursor(self.uri_2)

        cursor21.set_key(ds_1.key(50))
        cursor21.set_value(self.valueb)
        cursor22.set_key(ds_2.key(50))
        cursor22.set_value(self.valueb)

        self.assertEqual(cursor21.insert(), 0)
        self.assertEqual(cursor22.insert(), 0)

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start and acquire its snapshot before committing.
            ckpt_snapshot = 0
            while not ckpt_snapshot:
                time.sleep(1)
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_snapshot = stat_cursor[stat.conn.checkpoint_snapshot_acquired][2]
                stat_cursor.close()

            # commit the operations in out of order. Insert followed by truncate.
            session2.commit_transaction()
            session1.commit_transaction()

            # Evict all the modifications of the table1 before checkpoint gets into the table.
            self.evict_cursor(self.uri_1, ds_1, self.nrows)

        finally:
            done.set()
            ckpt.join()

        # Perform an additional checkpoint to ensure table2 also has the latest data.
        self.session.checkpoint()
        self.perform_backup_or_crash_restart(".", self.backup_dir)

        # Check that the both tables contains the last re-inserted value.
        cursor11 = self.session.open_cursor(self.uri_1)
        cursor12 = self.session.open_cursor(self.uri_2)

        cursor11.set_key(ds_1.key(50))
        self.assertEqual(cursor11.search(), 0)
        self.assertEqual(cursor11.get_value(), self.valueb)

        cursor12.set_key(ds_2.key(50))
        self.assertEqual(cursor12.search(), 0)
        self.assertEqual(cursor12.get_value(), self.valueb)
