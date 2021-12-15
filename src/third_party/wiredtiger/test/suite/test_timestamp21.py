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

# test_timestamp21.py
# Test read timestamp configuration that allows read timestamp to be older than oldest.
class test_timestamp21(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    def test_timestamp21(self):
        uri = 'table:test_timestamp21'
        self.session.create(uri, 'key_format=i,value_format=i')
        session2 = self.setUpSessionOpen(self.conn)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        cursor2 = session2.open_cursor(uri)

        # Insert first value at timestamp 10.
        self.session.begin_transaction()
        cursor[1] = 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Begin a read transaction at timestamp 5.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(5))

        # Move the oldest timestamp beyond the currently open transactions read timestamp.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(8))

        # Begin a transaction with a read timestamp of 6 and read_before_oldest specified.
        self.assertEqual(session2.begin_transaction(
            'read_timestamp=' + self.timestamp_str(6) + ',read_before_oldest=true'), 0)
        session2.rollback_transaction()

        # Begin a transaction with a read timestamp of 6 and no additional config. Check for
        # informational message output when a read timestamp is older than the oldest timestamp.
        if wiredtiger.standalone_build():
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
            session2.begin_transaction('read_timestamp=' + self.timestamp_str(6)))
        else:
            # This is a MongoDB message, not written in standalone builds.
            with self.expectedStdoutPattern('less than the oldest timestamp'):
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                session2.begin_transaction('read_timestamp=' + self.timestamp_str(6)))

        # Begin a transaction with the config specified but no read timestamp.
        session2.begin_transaction('read_before_oldest=true')
        # Set a read timestamp behind the oldest timestamp.
        self.assertEqual(
            session2.timestamp_transaction('read_timestamp=' + self.timestamp_str(5)), 0)
        session2.rollback_transaction()

        # Begin a transaction with a read timestamp of 5 and read_before_oldest specified.
        self.assertEqual(session2.begin_transaction(
            'read_timestamp=' + self.timestamp_str(5) + ',read_before_oldest=true'), 0)
        session2.rollback_transaction()

        # Begin a transaction with a read timestamp of 4 and read_before_oldest specified.
        # We get a different stdout message in this scenario.
        if wiredtiger.standalone_build():
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                session2.begin_transaction('read_timestamp=' +\
                self.timestamp_str(4) + ',read_before_oldest=true'))
        else:
            # This is a MongoDB message, not written in standalone builds.
            with self.expectedStdoutPattern('less than the pinned timestamp'):
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                    session2.begin_transaction('read_timestamp=' +\
                    self.timestamp_str(4) + ',read_before_oldest=true'))

        # Begin a transaction with a read timestamp of 6 and read_before_oldest off, this will
        # have the same behaviour as not specifying it.
        if wiredtiger.standalone_build():
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                session2.begin_transaction('read_timestamp=' +\
                self.timestamp_str(6) + ',read_before_oldest=false'))
        else:
            # This is a MongoDB message, not written in standalone builds.
            with self.expectedStdoutPattern('less than the oldest timestamp'):
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda:
                    session2.begin_transaction('read_timestamp=' +\
                    self.timestamp_str(6) + ',read_before_oldest=false'))

        # Expect an error when we use roundup timestamps alongside allow read timestamp before
        # oldest.
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: session2.begin_transaction(
            'read_timestamp=' + self.timestamp_str(6) +
            ',read_before_oldest=true,roundup_timestamps=(read)'),
            '/cannot specify roundup_timestamps.read and read_before_oldest/')
