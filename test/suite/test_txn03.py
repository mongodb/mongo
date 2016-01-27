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
#
# test_txn03.py
#   Transactions: using multiple cursor and session handles
#

import wiredtiger, wttest
from wtscenario import check_scenarios

class test_txn03(wttest.WiredTigerTestCase):
    tablename = 'test_txn03'
    uri1 = 'table:' + tablename + "_1"
    uri2 = 'table:' + tablename + "_2"
    key_str = "TEST_KEY1"
    data_str1 = "VAL"
    data_str2 = "TEST_VAL1"

    nentries = 1000
    scenarios = check_scenarios([
        ('var', dict(create_params = "key_format=S,value_format=S")),
    ])

    def test_ops(self):
        self.session.create(self.uri1, self.create_params)
        self.session.create(self.uri2, self.create_params)
        # Set up the table with entries for 1 and 10
        # We use the overwrite config so insert can update as needed.
        c = self.session.open_cursor(self.uri1, None, 'overwrite')
        c[self.key_str] = self.data_str1
        c.close()
        c = self.session.open_cursor(self.uri2, None, 'overwrite')
        c[self.key_str] = self.data_str1
        c.close()

        # Update the first table - this update should be visible in the
        # new session.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri1, None, 'overwrite')
        c[self.key_str] = self.data_str2
        self.session.commit_transaction()
        c.close()

        # Open another session and some transactional cursors.
        self.session2 = self.conn.open_session()
        self.session2.begin_transaction("isolation=snapshot")
        t1c = self.session2.open_cursor(self.uri1, None, 'overwrite')
        t2c = self.session2.open_cursor(self.uri2, None, 'overwrite')

        # Make an update in the first session.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri2, None, 'overwrite')
        c[self.key_str] = self.data_str2
        self.session.commit_transaction()
        c.close()

        t1c.set_key(self.key_str)
        t1c.search()
        t2c.set_key(self.key_str)
        t2c.search()
        self.assertEqual(t1c.get_value(), self.data_str2)
        self.assertEqual(t2c.get_value(), self.data_str1)

        # Clean up
        t1c.close()
        t2c.close()
        self.session2.rollback_transaction()

if __name__ == '__main__':
    wttest.run()
