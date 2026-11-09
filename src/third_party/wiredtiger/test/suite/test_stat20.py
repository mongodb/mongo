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

import wttest
from wiredtiger import stat

# The open cursor count follows cursors opened, cached, reopened and closed in another session.
class test_stat20(wttest.WiredTigerTestCase):
    # Statistics logging opens cursors in the background.
    conn_config = 'statistics=(fast),statistics_log=(wait=0)'
    uri = 'table:test_stat20'

    def open_cursor_count(self):
        return self.get_stat(stat.conn.cursor_open_count)

    def test_open_cursor_count(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        before = self.open_cursor_count()

        session = self.conn.open_session()
        start = self.open_cursor_count()
        cursor = session.open_cursor(self.uri)
        opened = self.open_cursor_count() - start
        self.assertGreater(opened, 0)
        cursor.close()
        self.assertEqual(self.open_cursor_count(), start)
        cursor = session.open_cursor(self.uri)
        self.assertEqual(self.open_cursor_count(), start + opened)
        session.close()
        self.assertEqual(self.open_cursor_count(), before)
