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
from helper import simulate_crash_restart
from wiredtiger import stat

# test_truncate27.py
# Test that unstable updates preceding a stable fast truncate operation are restored with the correct
# transaction IDs.
class test_truncate27(wttest.WiredTigerTestCase):

    def evict_cursor(self, uri, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def get_fast_truncated_pages(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        stat_cursor.close()
        return pages

    def test_truncate27(self):
        nrows = 100000
        uri = 'table:test_truncate27'
        self.session.create(uri, 'key_format=r,value_format=S')

        value = 'a' * 5

        # Pin oldest timestamp.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            # Insert some data.
            self.session.begin_transaction()
            cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
        cursor.close()

        # Update stable timestamp and checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows))
        self.session.checkpoint()

        # Evict everything.
        self.evict_cursor(uri, nrows)
        self.session.checkpoint()

        # Do a fast truncate.
        ts = nrows + 1
        truncate_session = self.conn.open_session()
        truncate_session.begin_transaction()
        cursor_start = truncate_session.open_cursor(uri)
        cursor_start.set_key(nrows // 2)
        truncate_session.truncate(None, cursor_start, None, None)
        truncate_session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        fast_truncates_pages = self.get_fast_truncated_pages()
        self.assertGreater(fast_truncates_pages, 0)
        cursor_start.close()

        # Make the fast truncate stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts + 1))

        # Insert unstable data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        cursor[100] = value
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts + 2))

        self.session.checkpoint()

        simulate_crash_restart(self, ".", "RESTART")
