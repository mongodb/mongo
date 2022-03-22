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
# test_timestamp08.py
#   Timestamps: API
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

class test_timestamp08(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp08'
    uri = 'table:' + tablename

    def test_timestamp_api(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri)

        # Begin by adding some data.
        self.session.begin_transaction()
        c[1] = 1
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(1))

        # Can set a zero timestamp. These calls shouldn't raise any errors.
        self.session.begin_transaction()
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 0)
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 0)
        self.session.rollback_transaction()

        # In a single transaction it is illegal to set a commit timestamp
        # older than the first commit timestamp used for this transaction.
        # Check both timestamp_transaction_uint and commit_transaction APIs.
        self.session.begin_transaction()
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 3)
        c[3] = 3
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 2),
                '/older than the first commit timestamp/')
        self.session.rollback_transaction()

        # Commit timestamp > Oldest timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(3))

        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 2),
                '/less than the oldest timestamp/')
        self.session.rollback_transaction()

        self.session.begin_transaction()
        c[4] = 4
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(4))

        # Commit timestamp > Stable timestamp.
        # Check both timestamp_transaction and commit_transaction APIs.
        # Oldest and stable timestamp are set to 5 at the moment.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(6))
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 5),
                '/after the stable timestamp/')
        self.session.rollback_transaction()

        # When explicitly set, commit timestamp for a transaction can be earlier
        # than the commit timestamp of an earlier transaction.
        self.session.begin_transaction()
        c[6] = 6
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 7)
        self.session.commit_transaction()

        self.session.begin_transaction()
        c[8] = 8
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 8)
        self.session.commit_transaction()

        self.session.begin_transaction()
        c[7] = 7
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 7)
        self.session.commit_transaction()

        # Read timestamp >= oldest timestamp
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(7) +
            ',stable_timestamp=' + self.timestamp_str(7))
        if wiredtiger.standalone_build():
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                self.session.begin_transaction('read_timestamp=' + self.timestamp_str(6)))
        else:
            # This is a MongoDB message, not written in standalone builds.
            with self.expectedStdoutPattern('less than the oldest timestamp'):
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                    self.session.begin_transaction('read_timestamp=' + self.timestamp_str(6)))

        # c[8] is not visible at read_timestamp < 8
        self.session.begin_transaction()
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 7)
        self.assertEqual(c[6], 6)
        self.assertEqual(c[7], 7)
        c.set_key(8)
        self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        self.session.begin_transaction()
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 8)
        self.assertEqual(c[6], 6)
        self.assertEqual(c[7], 7)
        self.assertEqual(c[8], 8)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), self.timestamp_str(8))
        self.session.commit_transaction()

        # We can move the oldest timestamp backwards with "force"
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) + ',force')
        if wiredtiger.standalone_build():
            self.session.begin_transaction()
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 4))
        else:
            # This is a MongoDB message, not written in standalone builds.
            self.session.begin_transaction()
            with self.expectedStdoutPattern('less than the oldest timestamp'):
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                    self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 4))
        self.session.rollback_transaction()

        self.session.begin_transaction()
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_READ, 6)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), self.timestamp_str(6))
        self.session.commit_transaction()

    def test_all_durable(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        cur1 = self.session.open_cursor(self.uri)

        # Since this is a non-prepared transaction, we'll be using the commit
        # timestamp when calculating all_durable since it's implied that they're
        # the same thing.
        self.session.begin_transaction()
        cur1[1] = 1
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 3)
        self.session.commit_transaction()
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '3')

        # We have a running transaction with a lower commit_timestamp than we've
        # seen before. So all_durable should return (lowest commit timestamp - 1).
        self.session.begin_transaction()
        cur1[2] = 2
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 2)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '1')
        self.session.commit_transaction()

        # After committing, go back to the value we saw previously.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '3')

        # For prepared transactions, we take into account the durable timestamp
        # when calculating all_durable.
        self.session.begin_transaction()
        cur1[3] = 3
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_PREPARE, 6)
        self.session.prepare_transaction()

        # If we have a commit timestamp for a prepared transaction, then we
        # don't want that to be visible in the all_durable calculation.
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 7)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '3')

        # Now take into account the durable timestamp.
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_DURABLE, 8)
        self.session.commit_transaction()
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '8')

        # All durable moves back when we have a running prepared transaction
        # with a lower durable timestamp than has previously been committed.
        self.session.begin_transaction()
        cur1[4] = 4
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_PREPARE, 3)
        self.session.prepare_transaction()

        # If we have a commit timestamp for a prepared transaction, then we
        # don't want that to be visible in the all_durable calculation.
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 4)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '8')

        # Now take into account the durable timestamp.
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_DURABLE, 5)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '4')
        self.session.commit_transaction()

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '8')

        # Now test a scenario with multiple commit timestamps for a single txn.
        self.session.begin_transaction()
        cur1[5] = 5
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 6)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '5')

        # Make more changes and set a new commit timestamp.
        # Our calculation should use the first commit timestamp so there should
        # be no observable difference to the all_durable value.
        cur1[6] = 6
        self.session.timestamp_transaction_uint(wiredtiger.WT_TS_TXN_TYPE_COMMIT, 7)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '5')

        # Once committed, we go back to 8.
        self.session.commit_transaction()
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_durable'), '8')

if __name__ == '__main__':
    wttest.run()
