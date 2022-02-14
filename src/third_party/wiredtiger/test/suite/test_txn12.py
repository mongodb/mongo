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
from suite_subprocess import suite_subprocess

# test_txn12.py
#    test of commit following failed op in a read only transaction.
class test_txn12(wttest.WiredTigerTestCase, suite_subprocess):
    name = 'test_txn12'
    uri = 'table:' + name
    create_params = 'key_format=i,value_format=i'

    # Test that read-only transactions can commit following a failure.
    def test_txn12(self):

        # Setup the session and table.
        session = self.conn.open_session(None)
        session.create(self.uri, self.create_params)
        session.begin_transaction()

        # Create a read only transaction.
        c = session.open_cursor(self.uri, None)
        c.next()
        msg = '/next_random.*boolean/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:session.open_cursor(self.uri, None, "next_random=bar"), msg)
        # This commit should succeed as open cursor should not set transaction
        # error.
        session.commit_transaction()

        # Create a read/write transaction.
        session.begin_transaction()
        c = session.open_cursor(self.uri, None)
        c[123] = 123
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:session.open_cursor(self.uri, None, "next_random=bar"), msg)
        # This commit should succeed as open cursor should not set transaction
        # error.
        session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
