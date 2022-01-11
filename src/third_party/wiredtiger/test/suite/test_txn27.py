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

import wiredtiger, wttest, time
from wtdataset import SimpleDataSet

# test_txn27.py
#   Test that the API returning a rollback error sets the reason for the rollback.
class test_txn27(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=1MB'

    def test_rollback_reason(self):
        uri = "table:txn27"
        # Create a very basic table.
        ds = SimpleDataSet(self, uri, 10, key_format='S', value_format='S')
        ds.populate()

        # Update key 5 in the first session.
        session1 = self.session
        cursor1 = session1.open_cursor(uri)
        session1.begin_transaction()
        cursor1[ds.key(5)] = "aaa"

        # Update the same key in the second session, expect a conflict error to be produced.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        cursor2.set_key(ds.key(5))
        cursor2.set_value("bbb")
        msg1 = '/conflict between concurrent operations/'
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor2.update(), msg1)
        self.assertEquals('/' + session2.get_rollback_reason() + '/', msg1)

        # Rollback the transactions, check that session2's rollback error was cleared.
        session2.rollback_transaction()
        self.assertEquals(session2.get_rollback_reason(), None)
        session1.rollback_transaction()

        # Start a new transaction and insert a value far too large for cache.
        session1.begin_transaction()
        cursor1.set_key(ds.key(1))
        cursor1.set_value("a"*1024*5000)
        self.assertEqual(0, cursor1.update())

        # Let WiredTiger's accounting catch up.
        time.sleep(2)

        # Attempt to insert another value with the same transaction. This will result in the
        # application thread being pulled into eviction and getting rolled back.
        cursor1.set_key(ds.key(2))
        cursor1.set_value("b"*1024)

        # This is the message that we expect to be raised when a thread is rolled back due to
        # cache pressure.
        msg2 = 'oldest pinned transaction ID rolled back for eviction'
        # Expect stdout to give us the true reason for the rollback.
        with self.expectedStdoutPattern(msg2):
            # This reason is the default reason for WT_ROLLBACK errors so we need to catch it.
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor1.update(), msg1)
        # Expect the rollback reason to give us the true reason for the rollback.
        self.assertEquals(session1.get_rollback_reason(), msg2)

if __name__ == '__main__':
    wttest.run()
