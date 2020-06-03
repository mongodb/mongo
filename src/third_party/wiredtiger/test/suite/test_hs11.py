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

# test_hs11.py
# Ensure that when we delete a key due to a tombstone being globally visible, we delete its
# associated history store content.
class test_hs11(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'

    def test_key_deletion_clears_hs(self):
        uri = 'table:test_hs11'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)

        value1 = 'a' * 500
        value2 = 'b' * 500

        # Apply a series of updates from timestamps 1-4.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        for ts in range(1, 5):
            for i in range(1, 10000):
                self.session.begin_transaction()
                cursor[str(i)] = value1
                self.session.commit_transaction('commit_timestamp=' + timestamp_str(ts))

        # Reconcile and flush versions 1-3 to the history store.
        self.session.checkpoint()

        # Apply a non-timestamped tombstone. When the pages get evicted, the keys will get deleted
        # since the tombstone is globally visible.
        for i in range(1, 10000):
            if i % 2 == 0:
                cursor.set_key(str(i))
                cursor.remove()

        # Now apply an update at timestamp 10 to recreate each key.
        for i in range(1, 10000):
            self.session.begin_transaction()
            cursor[str(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(10))

        # Ensure that we blew away history store content.
        for ts in range(1, 5):
            self.session.begin_transaction('read_timestamp=' + timestamp_str(ts))
            for i in range(1, 10000):
                if i % 2 == 0:
                    cursor.set_key(str(i))
                    self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
                else:
                    self.assertEqual(cursor[str(i)], value1)
            self.session.rollback_transaction()
