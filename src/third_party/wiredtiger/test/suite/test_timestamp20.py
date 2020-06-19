#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

def timestamp_str(t):
    return '%x' % t

# test_timestamp20.py
# Exercise fixing up of out-of-order updates in the history store.
class test_timestamp20(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'

    def test_timestamp20(self):
        uri = 'table:test_timestamp20'
        self.session.create(uri, 'key_format=S,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        value1 = 'a' * 500
        value2 = 'b' * 500
        value3 = 'c' * 500
        value4 = 'd' * 500
        value5 = 'e' * 500

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(10))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(20))

        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(30))

        old_reader_session = self.conn.open_session()
        old_reader_cursor = old_reader_session.open_cursor(uri)
        old_reader_session.begin_transaction('read_timestamp=' + timestamp_str(30))

        # Now put two updates out of order. 5 will go to the history store and will trigger a
        # correction to the existing contents.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value4
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(5))
            self.session.begin_transaction()
            cursor[str(i)] = value5
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(40))

        self.session.begin_transaction('read_timestamp=' + timestamp_str(30))
        for i in range(1, 10000):
            self.assertEqual(cursor[str(i)], value4)
        self.session.rollback_transaction()

        for i in range(1, 10000):
            self.assertEqual(old_reader_cursor[str(i)], value3)
        old_reader_session.rollback_transaction()
