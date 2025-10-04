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

import wiredtiger, time, errno, threading
from wttest import open_cursor
from error_info_util import error_info_util

# test_error_info03.py
#   Test that the get_last_error() session API returns the last error to occur in the session,
#   for the EBUSY during drop cases.
class test_error_info03(error_info_util):
    conn_config = 'timing_stress_for_test=[session_alter_slow,open_index_slow]'
    uri="table:test_error_info"

    def hold_checkpoint_and_schema_locks(self):
        session = self.conn.open_session()
        # The content of the new config is not important, it just needs to be non-empty (otherwise
        # the alter operation will abort).
        session.alter(self.uri, "access_pattern_hint=random")

    def hold_table_lock(self):
        session = self.conn.open_session()
        c = session.open_cursor(self.uri, None)
        c['key'] = 'value'
        c.close()

    def try_drop_no_wait(self):
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, "lock_wait=0")), "was expecting drop call to fail with EBUSY")

    def try_drop_no_wait_ignore_checkpoint_lock(self):
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, "lock_wait=0,checkpoint_wait=0")), "was expecting drop call to fail with EBUSY")

    def test_conflict_checkpoint(self):
        """
        Try to drop the table while another thread holds the checkpoint lock.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')

        lock_thread = threading.Thread(target=self.hold_checkpoint_and_schema_locks)
        drop_thread = threading.Thread(target=self.try_drop_no_wait)

        lock_thread.start()
        time.sleep(1)
        drop_thread.start()

        lock_thread.join()
        drop_thread.join()

        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_CONFLICT_CHECKPOINT_LOCK, "another thread is currently holding the checkpoint lock")

    def test_conflict_schema(self):
        """
        Try to drop the table while another thread holds the schema lock.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')

        lock_thread = threading.Thread(target=self.hold_checkpoint_and_schema_locks)
        drop_thread = threading.Thread(target=self.try_drop_no_wait_ignore_checkpoint_lock)

        lock_thread.start()
        time.sleep(1)
        drop_thread.start()

        lock_thread.join()
        drop_thread.join()

        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_CONFLICT_SCHEMA_LOCK, "another thread is currently holding the schema lock")

    def test_conflict_table(self):
        """
        Try to drop the table while another thread holds the table lock.
        """
        name = "test_error_info"
        self.uri = "table:" + name
        self.session.create(self.uri, 'key_format=S,value_format=S,columns=(k,v)')
        self.session.create('index:' + name + ':i0', 'columns=(k,v)')

        lock_thread = threading.Thread(target=self.hold_table_lock)
        drop_thread = threading.Thread(target=self.try_drop_no_wait)

        lock_thread.start()
        time.sleep(1)
        drop_thread.start()

        # Because the schema lock is acquired before the table lock, this drop will only fail with
        # WT_CONFLICT_TABLE_LOCK if the table lock is held by another thread but the schema lock is
        # available.
        # The function __schema_open_index happens to be one of the few locations where it's
        # possible to hold the table lock without also holding the schema lock, so the timing stress
        # has been placed there (at the cost of an additional wait, since it is also called when the
        # table is created).

        lock_thread.join()
        drop_thread.join()

        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_CONFLICT_TABLE_LOCK, "another thread is currently holding the table lock")

    def test_conflict_backup(self):
        """
        Open a backup cursor on a new table, then attempt to drop the table.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor('backup:', None, None)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_CONFLICT_BACKUP, "the table is currently performing backup and cannot be dropped")

    def test_conflict_dhandle(self):
        """
        Open a cursor on a new table, then attempt to drop the table.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_CONFLICT_DHANDLE, "another thread is currently holding the data handle of the table")

    def test_uncommitted_data(self):
        """
        Try to drop a table while it still has uncommitted data.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        with open_cursor(self.session, self.uri) as cursor:
            self.session.begin_transaction()
            cursor.set_key('key')
            cursor.set_value('value')
            self.assertEqual(cursor.update(), 0)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_UNCOMMITTED_DATA, "the table has uncommitted data and cannot be dropped yet")

    def test_dirty_data(self):
        """
        Try to drop a table without first performing a checkpoint.
        """
        self.session.create(self.uri, 'key_format=S,value_format=S')
        with open_cursor(self.session, self.uri) as cursor:
            self.session.begin_transaction()
            cursor.set_key('key')
            cursor.set_value('value')
            self.assertEqual(cursor.update(), 0)
            self.assertEqual(self.session.commit_transaction(), 0)

        # Give time for the oldest id to update before dropping the table.
        time.sleep(1)
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, None)), "was expecting drop call to fail with EBUSY")
        self.assert_error_equal(errno.EBUSY, wiredtiger.WT_DIRTY_DATA, "the table has dirty data and can not be dropped yet")
