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

import time, wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

# test_hs15.py
# Ensure eviction doesn't clear the history store again after checkpoint has done so because of the same update without timestamp.
class test_hs14(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=5MB'
    session_config = 'isolation=snapshot'

    def test_hs15(self):
        uri = 'table:test_hs15'
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri)

        value1 = 'a' * 500
        value2 = 'b' * 500
        value3 = 'c' * 500

        # Insert an update without timestamp
        self.session.begin_transaction()
        cursor[str(0)] = value1
        self.session.commit_transaction()

        # Insert a bunch of other contents to trigger eviction
        for i in range(1, 1000):
            self.session.begin_transaction()
            cursor[str(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(3))

        # Do a modify and an update with timestamps
        self.session.begin_transaction()
        cursor.set_key(str(0))
        mods = [wiredtiger.Modify('B', 100, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(1))

        self.session.begin_transaction()
        cursor[str(0)] = value2
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(2))

        # Make the modify with timestamp and the update without timestamp obsolete
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))

        # Do a checkpoint
        self.session.checkpoint()

        self.session.begin_transaction()
        cursor[str(0)] = value3
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(3))

        # Insert a bunch of other contents to trigger eviction
        for i in range(1, 1000):
            self.session.begin_transaction()
            cursor[str(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(3))

        expected = list(value1)
        expected[100] = 'B'
        expected = str().join(expected)
        self.session.begin_transaction('read_timestamp=' + timestamp_str(1))
        self.assertEqual(cursor[str(0)], expected)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + timestamp_str(2))
        self.assertEqual(cursor[str(0)], value2)
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp=' + timestamp_str(3))
        self.assertEqual(cursor[str(0)], value3)
        self.session.rollback_transaction()
