#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
#   Timestamps: API usage generates expected error with timestamps disabled
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_timestamp08(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp08'
    uri = 'table:' + tablename

    def test_timestamp_disabled(self):
        if wiredtiger.timestamp_build():
            self.skipTest('requires a non-timestamp build')

        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri)

        # Begin by adding some data
        self.session.begin_transaction()
        c[1] = 1
        self.session.commit_transaction()

        # setting a read_timestamp on a txn should fail
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.begin_transaction(
                'read_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')

        # setting a commit_timestamp on a txn should fail
        self.session.begin_transaction()
        c[2] = 2
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction(
                'commit_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')
        self.session.rollback_transaction()

        # committing a txn with commit_timestamp should fail
        self.session.begin_transaction()
        c[2] = 2
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(
                'commit_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')

        # query_timestamp should fail
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.query_timestamp(),
                '/requires a version of WiredTiger built with timestamp support/')

        # settings various timestamps should fail
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'oldest_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'stable_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'commit_timestamp=' + timestamp_str(1)),
                '/requires a version of WiredTiger built with timestamp support/')

if __name__ == '__main__':
    wttest.run()
