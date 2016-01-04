#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
from wtscenario import check_scenarios

# test_txn01.py
#    Transactions: basic functionality
class test_txn01(wttest.WiredTigerTestCase):
    nentries = 1000
    scenarios = check_scenarios([
        ('col-f', dict(uri='file:text_txn01',key_format='r',value_format='S')),
        ('col-t', dict(uri='table:text_txn01',key_format='r',value_format='S')),
        ('fix-f', dict(uri='file:text_txn01',key_format='r',value_format='8t')),
        ('fix-t', dict(uri='table:text_txn01',key_format='r',value_format='8t')),
        ('row-f', dict(uri='file:text_txn01',key_format='S',value_format='S')),
        ('row-t', dict(uri='table:text_txn01',key_format='S',value_format='S')),
    ])

    # Return the number of records visible to the cursor.
    def cursor_count(self, cursor):
        count = 0
        # Column-store appends result in phantoms, ignore records unless they
        # have our flag value.
        for r in cursor:
            if self.value_format == 'S' or cursor.get_value() == 0xab:
                count += 1
        return count

    # Checkpoint the database and assert the number of records visible to the
    # checkpoint matches the expected value.
    def check_checkpoint(self, expected):
        s = self.conn.open_session()
        s.checkpoint("name=test")
        cursor = s.open_cursor(self.uri, None, "checkpoint=test")
        self.assertEqual(self.cursor_count(cursor), expected)
        s.close()

    # Open a cursor with snapshot isolation, and assert the number of records
    # visible to the cursor matches the expected value.
    def check_txn_cursor(self, level, expected):
        s = self.conn.open_session()
        cursor = s.open_cursor(self.uri, None)
        s.begin_transaction(level)
        self.assertEqual(self.cursor_count(cursor), expected)
        s.close()

    # Open a session with snapshot isolation, and assert the number of records
    # visible to the cursor matches the expected value.
    def check_txn_session(self, level, expected):
        s = self.conn.open_session(level)
        cursor = s.open_cursor(self.uri, None)
        s.begin_transaction()
        self.assertEqual(self.cursor_count(cursor), expected)
        s.close()

    def check(self, cursor, committed, total):
        # The cursor itself should see all of the records.
        if cursor != None:
            cursor.reset()
            self.assertEqual(self.cursor_count(cursor), total)

        # Read-uncommitted should see all of the records.
        # Snapshot and read-committed should see only committed records.
        self.check_txn_cursor('isolation=read-uncommitted', total)
        self.check_txn_session('isolation=read-uncommitted', total)

        self.check_txn_cursor('isolation=snapshot', committed)
        self.check_txn_session('isolation=snapshot', committed)

        self.check_txn_cursor('isolation=read-committed', committed)
        self.check_txn_session('isolation=read-committed', committed)

        # Checkpoints should only write committed items.
        self.check_checkpoint(committed)

    # Loop through a set of inserts, periodically committing; before each
    # commit, verify the number of visible records matches the expected value.
    def test_visibility(self):
        self.session.create(self.uri,
            'key_format=' + self.key_format +
            ',value_format=' + self.value_format)

        committed = 0
        cursor = self.session.open_cursor(self.uri, None)
        self.check(cursor, 0, 0)
        self.session.begin_transaction()
        for i in xrange(self.nentries):
            if i > 0 and i % (self.nentries / 37) == 0:
                self.check(cursor, committed, i)
                self.session.commit_transaction()
                committed = i
                self.session.begin_transaction()

            if self.key_format == 'S':
                cursor.set_key("key: %06d" % i)
            else:
                cursor.set_key(i + 1)
            if self.value_format == 'S':
                cursor.set_value("value: %06d" % i)
            else:
                cursor.set_value(0xab)
            cursor.insert()

        self.check(cursor, committed, self.nentries)
        self.session.commit_transaction()
        self.check(cursor, self.nentries, self.nentries)


# Test that read-committed is the default isolation level.
class test_read_committed_default(wttest.WiredTigerTestCase):
    uri = 'table:test_txn'

    # Return the number of records visible to the cursor.
    def cursor_count(self, cursor):
        count = 0
        for r in cursor:
            count += 1
        return count

    def test_read_committed_default(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor['key: aaa'] = 'value: aaa'
        self.session.commit_transaction()
        self.session.begin_transaction()
        cursor['key: bbb'] = 'value: bbb'

        s = self.conn.open_session()
        cursor = s.open_cursor(self.uri, None)
        s.begin_transaction("isolation=read-committed")
        self.assertEqual(self.cursor_count(cursor), 1)
        s.commit_transaction()
        s.begin_transaction(None)
        self.assertEqual(self.cursor_count(cursor), 1)
        s.commit_transaction()
        s.close()


if __name__ == '__main__':
    wttest.run()
