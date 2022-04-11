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

        # In the case of named checkpoints, verify they're still there,
        # reflecting an empty file.
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

if __name__ == '__main__':
    wttest.run()
