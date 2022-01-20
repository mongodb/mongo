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
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint_snapshot05.py
#   This test is to run checkpoint and eviction in parallel with timing
#   stress for checkpoint and let eviction write more data than checkpoint
#   after a bulk load on a table to check the backup recovery.
class test_checkpoint_snapshot05(wttest.WiredTigerTestCase):
    # Create a table.
    uri = "table:test_checkpoint_snapshot05"
    backup_dir = "BACKUP"

    format_values = [
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true),timing_stress_for_test=[checkpoint_slow]'
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

    def check(self, check_value, uri, nrows):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if self.value_format == '8t':
                self.assertEqual(v, check_value)
            else:
                self.assertEqual(v, check_value + str(count + 1))
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)

    def evict(self, uri, ds, nrows):
        s = self.conn.open_session()
        s.begin_transaction()
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(ds.key(i))
            self.assertEquals(evict_cursor.search(), 0)
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()
        s.close()

    def test_checkpoint_snapshot(self):
        self.moresetup()

        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config='log=(enabled=false)'+self.extraconfig)
        ds.populate()

        cursor = self.session.open_cursor(self.uri, None, "bulk")
        for i in range(1, self.nrows + 1):
            if self.value_format == '8t':
                cursor[i] = self.valuea
            else:
                cursor[i] = self.valuea + str(i)
        cursor.close()

        self.check(self.valuea, self.uri, self.nrows)

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)

        for i in range(1, self.nrows + 1):
            cursor1.set_key(ds.key(i))
            if self.value_format == '8t':
                cursor1.set_value(self.valueb)
            else:
                cursor1.set_value(self.valueb + str(i))
            cursor1.set_value(self.valueb)
            self.assertEqual(cursor1.update(), 0)

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()
            # Sleep for sometime so that checkpoint starts before committing last transaction.
            time.sleep(2)
            session1.commit_transaction()
            self.evict(self.uri, ds, self.nrows)
        finally:
            done.set()
            ckpt.join()

        #Take a backup and restore it.
        self.take_full_backup(".", self.backup_dir)
        self.reopen_conn(self.backup_dir)

        # Check the table contains the last checkpointed value.
        self.check(self.valuea, self.uri, self.nrows)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertEqual(keys_removed, 0)

if __name__ == '__main__':
    wttest.run()
