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
from helper import simulate_crash_restart
from wiredtiger import stat
from wtbackup import backup_base

# test_backup18.py
# Test backup:query_id API.
class test_backup18(backup_base):
    conn_config= 'cache_size=1G,log=(enabled,file_max=100K)'
    pfx = 'test_backup'
    uri="table:test"

    def get_stat(self, stat_name):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_name][2]
        stat_cursor.close()
        return value

    def id_check(self, expect):
        got = []
        bkup_c = self.session.open_cursor('backup:query_id', None, None)
        # We cannot use 'for idstr in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            idstr = bkup_c.get_key()
            got.append(idstr)
        bkup_c.close()
        got.sort()
        expect.sort()
        self.assertEqual(got, expect)

    def test_backup18(self):
        # We're not taking actual backups in this test, but we do want a table to
        # exist for the backup cursor to generate something.
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data(self.uri, 'key', 'value', True)

        msg = "/is not configured/"
        self.pr("Query IDs before any backup")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEqual(self.session.open_cursor('backup:query_id',
            None, None), 0), msg)

        # Open up the backup cursor.
        def_gran = 16 * 1024 * 1024
        config = 'incremental=(enabled,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        self.assertEqual(1, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(1, self.get_stat(stat.conn.backup_incremental))
        self.assertEqual(def_gran, self.get_stat(stat.conn.backup_granularity))

        # Try to open the query cursor as a duplicate on the backup.
        msg = "/should be passed either/"
        self.pr("Query IDs as duplicate cursor")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEqual(self.session.open_cursor('backup:query_id',
            bkup_c, None), 0), msg)

        # Try to open the query cursor while backup cursor is open.
        msg = "/there is already a backup/"
        self.pr("Query IDs while backup cursor open")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEqual(self.session.open_cursor('backup:query_id',
            None, None), 0), msg)
        bkup_c.close()
        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(1, self.get_stat(stat.conn.backup_incremental))

        # Check a few basic cases.
        self.pr("Query IDs basic cases")
        expect = ["ID1"]
        self.id_check(expect)

        config = 'incremental=(enabled,src_id="ID1",this_id="ID2")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()
        expect = ["ID1", "ID2"]
        self.id_check(expect)

        config = 'incremental=(enabled,src_id="ID2",this_id="ID3")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()
        expect = ["ID2", "ID3"]
        self.id_check(expect)

        self.reopen_conn()
        self.pr("Query after reopen")
        expect = ["ID2", "ID3"]
        self.id_check(expect)

        # Force stop and then recheck. Incremental is no longer configured.
        msg = "/is not configured/"
        self.pr("Query after force stop")
        config = 'incremental=(force_stop=true)'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEqual(self.session.open_cursor('backup:query_id',
            None, None), 0), msg)
        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(0, self.get_stat(stat.conn.backup_incremental))

        # Open up an incremental backup cursor again.
        config = 'incremental=(enabled,this_id="ID1")'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()
        expect = ["ID1"]
        self.id_check(expect)

        # Force stop, and then simulate a crash. On restart, the incremental should not be
        # configured.
        config = 'incremental=(force_stop=true)'
        bkup_c = self.session.open_cursor('backup:', None, config)
        bkup_c.close()
        simulate_crash_restart(self, ".", "RESTART")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEqual(self.session.open_cursor('backup:query_id',
            None, None), 0), msg)
        self.assertEqual(0, self.get_stat(stat.conn.backup_cursor_open))
        self.assertEqual(0, self.get_stat(stat.conn.backup_incremental))
