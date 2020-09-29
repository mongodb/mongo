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

# test_hs16.py
# Ensure that we don't panic when inserting an update without timestamp to the history store.
class test_hs16(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=5MB'
    session_config = 'isolation=snapshot'

    def test_hs16(self):
        uri = 'table:test_hs16'
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri)

        # Insert an update without timestamp
        self.session.begin_transaction()
        cursor[str(0)] = 'a'
        self.session.commit_transaction()

        # Update an update at timestamp 1
        self.session.begin_transaction()
        cursor[str(0)] = 'b'
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(1))

        # Open anther session to make the next update without timestamp non-globally visible
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        cursor[str(1)] = 'a'

        # Update an update without timestamp
        self.session.begin_transaction()
        cursor[str(0)] = 'c'
        self.session.commit_transaction()

        # Update an update at timestamp 2
        self.session.begin_transaction()
        cursor[str(0)] = 'd'
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(2))

        # Do a checkpoint, it should not panic
        self.session.checkpoint()
