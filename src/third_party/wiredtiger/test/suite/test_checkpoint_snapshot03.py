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
#
# [TEST_TAGS]
# rollback_to_stable
# [END_TAGS]

import fnmatch, os, shutil, threading, time
from wtthread import checkpoint_thread, op_thread
from helper import simulate_crash_restart
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_checkpoint_snapshot03.py
#   This test is to check RTS skips the unnecessary pages when the table has more than the
#   checkpoint snapshot.
class test_checkpoint_snapshot03(wttest.WiredTigerTestCase):

    # Create a table.
    uri = "table:test_checkpoint_snapshot03"
    nrows = 500000

    format_values = [
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('string_row', dict(key_format='S', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'cache_size=250MB,statistics=(all),statistics_log=(json,on_close,wait=1),log=(enabled=true)'
        return config

    def large_updates(self, uri, value, ds, nrows):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def check(self, check_value, uri, nrows):
        # In FLCS the existence of the invisible extra row causes the table to extend
        # under it. Until that's fixed, expect (not just allow) this row to exist and
        # and demand it reads back as zero and not as check_value. When this behavior
        # is fixed (so the end of the table updates transactionally) the special-case
        # logic can just be removed.
        flcs_tolerance = self.value_format == '8t'

        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if flcs_tolerance and count >= nrows:
                self.assertEqual(v, 0)
            else:
                self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows + 1 if flcs_tolerance else nrows)

    def test_checkpoint_snapshot(self):

        ds = SimpleDataSet(self, self.uri, 0, \
                key_format=self.key_format, value_format=self.value_format, \
                config='log=(enabled=false),leaf_page_max=4k')
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
            valuec = 99
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100
            valuec = "ccccc" * 100

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)
        for i in range(self.nrows + 1, self.nrows + 2):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(valueb)
            self.assertEqual(cursor1.insert(), 0)

        self.large_updates(self.uri, valuea, ds, self.nrows)
        self.check(valuea, self.uri, self.nrows)

        self.session.checkpoint()
        session1.rollback_transaction()
        self.reopen_conn()

        # Check the table contains the last checkpointed value.
        self.session.breakpoint()
        self.check(valuea, self.uri, self.nrows)

        session1 = self.conn.open_session()
        session1.begin_transaction()
        cursor1 = session1.open_cursor(self.uri)
        for i in range(self.nrows + 1, self.nrows + 2):
            cursor1.set_key(ds.key(i))
            cursor1.set_value(valueb)
            self.assertEqual(cursor1.insert(), 0)

        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, 2):
            cursor.set_key(ds.key(i))
            cursor.set_value(valuec)
            self.assertEqual(cursor.update(), 0)
        self.session.commit_transaction()

        self.session.checkpoint()
        session1.rollback_transaction()

        # Simulate a server crash and restart.
        simulate_crash_restart(self, ".", "RESTART")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_skipped = stat_cursor[stat.conn.txn_rts_tree_walk_skip_pages][2]
        upd_aborted = stat_cursor[stat.conn.txn_rts_upd_aborted][2]
        stat_cursor.close()

        self.assertGreater(inconsistent_ckpt, 0)
        self.assertEqual(upd_aborted, 0)
        self.assertGreaterEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreater(pages_skipped, 0)

if __name__ == '__main__':
    wttest.run()
