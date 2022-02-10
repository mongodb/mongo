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
# test_isolation01.py
#   Transactions isolation mode: This test is to test for different isolation modes.
#   The API reset_snapshot should return error when called withread committed isolation mode
#   or when the session has performed any write operations.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

class test_isolation01(wttest.WiredTigerTestCase):

    uri = 'table:test_isolation01'
    iso_types = [
        ('isolation_read_uncommitted', dict(isolation='read-uncommitted')),
        ('isolation_read_committed', dict(isolation='read-committed')),
        ('isolation_snapshot', dict(isolation='snapshot'))
    ]
    scenarios = make_scenarios(iso_types)
    key = 'key'
    value = 'value'

    def test_isolation_level(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None)

        # Begin a transaction with different isolation levels.
        self.session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(self.key)
        cursor.set_value(self.value)
        # read committed and read uncommitted transactions are readonly, any write operations with
        # these isolation levels should throw an error.
        if self.isolation != 'snapshot':
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.insert(), "/not supported in read-committed or read-uncommitted transactions/")
        else:
            self.assertEqual(cursor.insert(), 0)

        if self.isolation == 'snapshot':
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.reset_snapshot(), "/only supported before .* modifications/")
        else:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.reset_snapshot(),
                "/not supported in read-committed or read-uncommitted transactions/")
        self.session.commit_transaction()

        cursor2 = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction('isolation=' + self.isolation)
        cursor2.set_key(self.key)
        cursor2.search()
        if self.isolation == 'snapshot':
            self.session.reset_snapshot()
        else:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.reset_snapshot(),
                "/not supported in read-committed or read-uncommitted transactions/")

if __name__ == '__main__':
    wttest.run()
