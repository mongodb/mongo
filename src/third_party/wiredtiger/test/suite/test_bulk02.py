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
# test_bulk02.py
#       Bulk-load testing.

import time
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios

# test_bulkload_checkpoint
#       Test bulk-load with checkpoints.
class test_bulkload_checkpoint(wttest.WiredTigerTestCase, suite_subprocess):
    types = [
        ('file', dict(uri='file:data')),
        ('table', dict(uri='table:data')),
    ]
    configs = [
        ('fix', dict(keyfmt='r', valfmt='8t')),
        ('var', dict(keyfmt='r', valfmt='S')),
        ('row', dict(keyfmt='S', valfmt='S')),
    ]
    ckpt_type = [
        ('named', dict(ckpt_type='named')),
        ('unnamed', dict(ckpt_type='unnamed')),
    ]

    scenarios = make_scenarios(types, configs, ckpt_type)

    # Bulk-load handles are skipped by checkpoints.
    # Named and unnamed checkpoint versions.
    def test_bulkload_checkpoint(self):
        # Open a bulk cursor and insert a few records.
        config = 'key_format={},value_format={}'.format(self.keyfmt, self.valfmt)
        self.session.create(self.uri, config)
        cursor = self.session.open_cursor(self.uri, None, 'bulk')
        for i in range(1, 10):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Checkpoint a few times (to test the drop code).
        for i in range(1, 5):
            if self.ckpt_type == 'named':
                self.session.checkpoint('name=myckpt')
            else:
                self.session.checkpoint()

        # Close the bulk cursor.
        cursor.close()

        # Because the checkpoint skipped the table (because of the open bulk cursor), the
        # checkpoint may exist (appears to) but the table isn't in it and can't be opened.
        if self.ckpt_type == 'named':
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(self.uri, None, 'checkpoint=myckpt'))

# test_bulkload_backup
#       Test bulk-load with hot-backup.
class test_bulkload_backup(wttest.WiredTigerTestCase, suite_subprocess):
    types = [
        ('file', dict(uri='file:data')),
        ('table', dict(uri='table:data')),
    ]
    configs = [
        ('fix', dict(keyfmt='r', valfmt='8t')),
        ('var', dict(keyfmt='r', valfmt='S')),
        ('row', dict(keyfmt='S', valfmt='S')),
    ]
    ckpt_type = [
        ('named', dict(ckpt_type='named')),
        ('none', dict(ckpt_type='none')),
        ('unnamed', dict(ckpt_type='unnamed')),
    ]
    session_type = [
        ('different', dict(session_type='different')),
        ('same', dict(session_type='same')),
    ]
    scenarios = make_scenarios(types, configs, ckpt_type, session_type)

    # Backup a set of chosen tables/files using the wt backup command.
    # The only files are bulk-load files, so they shouldn't be copied.
    def check_backup(self, session):
        backupdir = 'backup.dir'
        self.backup(backupdir, session)

        # Open the target directory, and confirm the object has no contents.
        conn = self.wiredtiger_open(backupdir)
        session = conn.open_session()
        cursor = session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        conn.close()

    def test_bulk_backup(self):
        # Open a bulk cursor and insert a few records.
        config = 'key_format={},value_format={}'.format(self.keyfmt, self.valfmt)
        self.session.create(self.uri, config)
        cursor = self.session.open_cursor(self.uri, None, 'bulk')
        for i in range(1, 10):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)

        # Test without a checkpoint, with an unnamed checkpoint, with a named
        # checkpoint.
        if self.ckpt_type == 'named':
            self.session.checkpoint('name=myckpt')
        elif self.ckpt_type == 'unnamed':
            self.session.checkpoint()

        # Test with the same and different sessions than the bulk-get call,
        # test both the database handle and session handle caches.
        if self.session_type == 'same':
            self.check_backup(self.session)
        else:
            self.check_backup(self.conn.open_session())



# test_bulk_checkpoint_in_txn
#       Test a bulk cursor with checkpoint while cursor and txn open.
class test_bulk_checkpoint_in_txn(wttest.WiredTigerTestCase, suite_subprocess):
    tablebase = 'test_bulk02'
    uri = 'table:' + tablebase

    def test_bulk_checkpoint_in_txn(self):
        # The following unusual sequence of operations will create an abort/crash without a check to prevent
        # opening a bulk cursor inside a transaction.
        #
        # Uncomment the whole of this method, and test it against a version of WiredTiger that does
        # not have the check against opening a bulk cursor inside a transaction to reproduce an abort.

        test_uri = '%s.%s' % (self.uri, "force_bulk_checkpoint_in_txn_test")

        self.session.create(test_uri, "")
        self.session.begin_transaction()
    #
    #     c = self.session.open_cursor(test_uri, None, 'bulk')
    #     for k in range(5):
    #         c["key{}".format(k)] = "value".format(k)
    #
    #     self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
    #                                  lambda: self.session.checkpoint('name=ckpt'),
    #                                  '/not permitted in a running transaction/')
    #
    #     c.close()
    #
    #     self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
    #                                  lambda: self.session.commit_transaction(),
    #                                  '/transaction requires rollback/')

    def test_bulk_cursor_in_txn(self):
        test_uri = '%s.%s' % (self.uri, "force_bulk_checkpoint_in_txn_test")

        self.session.create(test_uri, "")
        self.session.begin_transaction()

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                     lambda: self.session.open_cursor(test_uri, None, 'bulk'),
                                     "/Bulk cursors can't be opened inside a transaction/")
