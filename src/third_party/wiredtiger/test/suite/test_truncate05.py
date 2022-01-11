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

# test_truncate05.py
# Test various fast truncate visibility scenarios
class test_truncate05(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB'

    def test_truncate_read_older_than_newest(self):
        uri = 'table:test_truncate05'
        self.session.create(uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(uri)

        value1 = 'a' * 500
        value2 = 'b' * 500

        # Insert a range of keys.
        for i in range(1, 1000):
            self.session.begin_transaction()
            cursor[i] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Reopen the connection to force all content to disk.
        self.reopen_conn()
        self.session.create(uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(uri)

        # Insert a single update at a later timestamp.
        self.session.begin_transaction()
        cursor[500] = value2
        self.assertEqual(self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3)), 0)

        # Insert a bunch of other content to fill the database and evict the committed update.
        for i in range(1000, 20000):
            self.session.begin_transaction()
            cursor[i] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        # Start a transaction with an earlier read timestamp than the commit timestamp of the
        # previous update.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(2))

        # Truncate from key 1 to 1000.
        start = self.session.open_cursor(uri, None)
        start.set_key(1)
        end = self.session.open_cursor(uri, None)
        end.set_key(1000)

        # Call the truncate and expect failure.
        self.assertRaisesException(
            wiredtiger.WiredTigerError,lambda: self.session.truncate(None, start, end, None))
        self.session.rollback_transaction()
