#!/usr/bin/env python
#
# Public Domain 2014-2019 MongoDB, Inc.
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
# test_timestamp14.py
#   Global timestamps: oldest reader, all committed, pinned
#

import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

class test_timestamp14(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp14'
    uri = 'table:' + tablename
    session_config = 'isolation=snapshot'

    def test_all_committed(self):
        all_committed_uri = self.uri + '_all_committed'
        session1 = self.setUpSessionOpen(self.conn)
        session2 = self.setUpSessionOpen(self.conn)
        session1.create(all_committed_uri, 'key_format=i,value_format=i')
        session2.create(all_committed_uri, 'key_format=i,value_format=i')

        # Scenario 0: No commit timestamp has ever been specified therefore
        # There is no all_committed timestamp and we will get an error
        # Querying for it.
        session1.begin_transaction()
        cur1 = session1.open_cursor(all_committed_uri)
        cur1[1]=1
        session1.commit_transaction()
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=all_committed'))

        # Scenario 1: A single transaction with a commit timestamp, will
        # result in the all_committed timestamp being set.
        session1.begin_transaction()
        cur1[1]=1
        session1.commit_transaction('commit_timestamp=1')
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "1")

        # Scenario 2: A transaction begins and specifies that it intends
        # to commit at timestamp 2, a second transaction begins and commits
        # at timestamp 3.
        session1.begin_transaction()
        session1.timestamp_transaction('commit_timestamp=2')

        session2.begin_transaction()
        cur2 = session2.open_cursor(all_committed_uri)
        cur2[2] = 2
        session2.commit_transaction('commit_timestamp=3')

        # As the original transaction is still running the all_committed
        # timestamp is being held at 1.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "1")
        cur1[1] = 2
        session1.commit_transaction()

        # Now that the original transaction has finished the all_committed
        # timestamp has moved to 3, skipping 2 as there is a commit with
        # a greater timestamp already existing.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "3")

        # Senario 3: Commit with a commit timestamp of 5 and then begin a
        # transaction intending to commit at 4, the all_committed timestamp
        # should move back to 3. Until the transaction at 4 completes.
        session1.begin_transaction()
        cur1[1] = 3
        session1.commit_transaction('commit_timestamp=5')
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "5")

        session1.begin_transaction()
        # All committed will now move back to 3 as it is the point at which
        # all transactions up to that point have committed.
        session1.timestamp_transaction('commit_timestamp=4')

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "3")

        session1.commit_transaction()

        # Now that the transaction at timestamp 4 has completed the
        # all committed timestamp is back at 5.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "5")

        # Scenario 4: Holding a transaction open without a commit timestamp
        # Will not affect the all_committed timestamp.
        session1.begin_transaction()
        session2.begin_transaction()
        cur2[2] = 2
        session2.commit_transaction('commit_timestamp=6')

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "6")
        cur1[1] = 2
        session1.commit_transaction()

    def test_oldest_reader(self):
        oldest_reader_uri = self.uri + '_oldest_reader_pinned'
        session1 = self.setUpSessionOpen(self.conn)
        session2 = self.setUpSessionOpen(self.conn)
        session1.create(oldest_reader_uri, 'key_format=i,value_format=i')
        session2.create(oldest_reader_uri, 'key_format=i,value_format=i')

        # Nothing is reading so there is no oldest reader.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=oldest_reader'))

        # Write some data for reading.
        session1.begin_transaction()
        cur1 = session1.open_cursor(oldest_reader_uri)
        cur1[1]=1
        session1.commit_transaction('commit_timestamp=5')

        # No active sessions so no oldest reader.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=oldest_reader'))

        # Create an active read session.
        session1.begin_transaction('read_timestamp=5')
        # Oldest reader should now exist and be equal to our read timestamp.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), '5')

        # Start transaction without read timestamp specified
        # Should not affect the current oldest reader.
        session2.begin_transaction()
        cur2 = session2.open_cursor(oldest_reader_uri)
        cur2[2] = 2

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), '5')

        session2.commit_transaction('commit_timestamp=7')

        # Open read transaction with newer read timestamp, oldest
        # Reader should therefore be unchanged.
        session2.begin_transaction('read_timestamp=7')

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), '5')

        # End current oldest reader transaction, it will have now moved
        # up to our transaction created before.
        session1.commit_transaction()

        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), '7')

        session2.commit_transaction()

        # Now that all read transactions have completed we will be back
        # to having no oldest reader.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=oldest_reader'))

    def test_pinned_oldest(self):
        pinned_oldest_uri = self.uri + 'pinned_oldest'
        session1 = self.setUpSessionOpen(self.conn)
        session1.create(pinned_oldest_uri, 'key_format=i,value_format=i')
        # Confirm no oldest timestamp exists.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=oldest'))

        # Confirm no pinned timestamp exists.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=pinned'))

        # Write some data for reading.
        session1.begin_transaction()
        cur1 = session1.open_cursor(pinned_oldest_uri)
        cur1[1]=1
        session1.commit_transaction('commit_timestamp=5')

        # Confirm no oldest timestamp exists.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=oldest'))

        # Confirm no pinned timestamp exists.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp('get=pinned'))

        self.conn.set_timestamp('oldest_timestamp=5')

        # Pinned timestamp should now match oldest timestamp
        self.assertTimestampsEqual(self.conn.query_timestamp('get=pinned'), '5')

        # Write some more data for reading.
        session1.begin_transaction()
        cur1[2]=2
        session1.commit_transaction('commit_timestamp=8')

        # Create an active read session.
        session1.begin_transaction('read_timestamp=5')

        # Move oldest timestamp past active read session.
        self.conn.set_timestamp('oldest_timestamp=8')

        # Pinned timestamp should now reflect oldest reader.
        self.assertTimestampsEqual(self.conn.query_timestamp('get=pinned'), '5')

        # End active read session.
        session1.commit_transaction()

        # Pinned timestamp should now match oldest timestamp.
        self.assertTimestampsEqual(self.conn.query_timestamp('get=pinned'), '8')

    def test_all(self):
        all_uri = self.uri + 'pinned_oldest'
        session1 = self.setUpSessionOpen(self.conn)
        session2 = self.setUpSessionOpen(self.conn)
        session1.create(all_uri, 'key_format=i,value_format=i')
        session2.create(all_uri, 'key_format=i,value_format=i')
        cur1 = session1.open_cursor(all_uri)
        cur2 = session2.open_cursor(all_uri)
        # Set up oldest timestamp.
        self.conn.set_timestamp('oldest_timestamp=1')

        # Write some data for reading.

        session1.begin_transaction()
        cur1[1]=1
        session1.commit_transaction('commit_timestamp=2')
        session1.begin_transaction()
        cur1[1]=2
        session1.commit_transaction('commit_timestamp=4')
        # Confirm all_committed is now 4.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "4")

        # Create a read session.
        session1.begin_transaction('read_timestamp=2')
        # Confirm oldest reader is 2 and the the value we read is 1.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_reader'), "2")

        self.assertEqual(cur1[1], 1)
        # Commit some data at timestamp 7.
        session2.begin_transaction()
        cur2[2] = 2
        session2.commit_transaction('commit_timestamp=7')
        # All_committed should now be 7.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "7")

        # Move oldest to 5.
        self.conn.set_timestamp('oldest_timestamp=5')

        # Confirm pinned timestamp is pointing at oldest_reader.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=pinned'),
            self.conn.query_timestamp('get=oldest_reader'))

        # Begin a write transaction pointing at timestamp 6,
        # this is below our current all_committed so it should move back
        # to the oldest timestamp.
        session2.begin_transaction()
        session2.timestamp_transaction('commit_timestamp=6')
        cur2[2] = 3

        # Confirm all_committed is now equal to oldest.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'),
            self.conn.query_timestamp('get=oldest'))

        session2.commit_transaction()
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=all_committed'), "7")
        # End our read transaction.
        session1.commit_transaction()

        # Pinned will now match oldest.
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=pinned'),
            self.conn.query_timestamp('get=oldest'))

if __name__ == '__main__':
    wttest.run()
