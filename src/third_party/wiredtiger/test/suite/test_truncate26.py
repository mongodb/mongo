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
from wtdataset import SimpleDataSet

# test_cursor24.py
# Check that WiredTiger returns an error when a session uses cursors not owned by that session.

class test_cursor24(wttest.WiredTigerTestCase):
    uri = 'table:test_cursor24'
    nrows = 100

    def test_cursor24_truncate(self):
        ds = SimpleDataSet(self, self.uri, 0, key_format='i', value_format='S')
        ds.populate()

        # Insert some data to give our cursors something to position on.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows):
            cursor[ds.key(i)] = str(1)

        # Open the same start and stop cursors in sessions 1 and 2
        s1_start_cur = ds.open_cursor(self.uri, None)
        s1_start_cur.set_key(ds.key(50))
        s1_stop_cur = ds.open_cursor(self.uri, None)
        s1_stop_cur.set_key(ds.key(80))

        session2 = self.conn.open_session()
        s2_start_cur = session2.open_cursor(self.uri)
        s2_start_cur.set_key(ds.key(50))
        s2_stop_cur = session2.open_cursor(self.uri)
        s2_stop_cur.set_key(ds.key(80))

        # Now try all combinations of cursors on session 2. Any use of a session 1 cursor returns EINVAL.
        msg = '/bounding cursors must be owned by the truncating session: Invalid argument/'

        self.assertEqual(session2.truncate(None, s2_start_cur, s2_stop_cur, None), 0)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session2.truncate(None, s1_start_cur, s2_stop_cur, None), msg)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session2.truncate(None, s2_start_cur, s1_stop_cur, None), msg)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session2.truncate(None, s1_start_cur, s1_stop_cur, None), msg)
