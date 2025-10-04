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
from helper import simulate_crash_restart

# test_truncate28.py
# Test that out of order commit timestamps aren't allowed when performing truncate operations.
class test_truncate28(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'
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

    def test_truncate28(self):
        if wiredtiger.diagnostic_build():
            self.skipTest('requires a non-diagnostic build')

        if not wiredtiger.standalone_build():
            self.skipTest('requires a standalone build')

        nrows = 10000
        uri = 'table:test_truncate27'
        self.session.create(uri, 'key_format=i,value_format=S')

        value = 'a' * 100

        # Pin oldest timestamp.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            # Insert some data.
            self.session.begin_transaction()
            cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
        cursor.close()

        # Insert a prepared committed data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        cursor[nrows + 1] = value
        cursor.close()
        prepare_ts = nrows + 1
        commit_ts = nrows + 1
        durable_ts = nrows + 2
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(prepare_ts))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(durable_ts))
        self.session.commit_transaction()

        # Update stable timestamp and checkpoint.
        stable_ts = (int) (nrows / 2)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable_ts))
        self.session.checkpoint()

        # Evict everything.
        self.evict_cursor(uri, nrows + 1)
        self.session.checkpoint()

        # Do a fast truncate with a commit timestamp that is less than the previous durable timestamp
        # on the page.
        truncate_ts = nrows
        truncate_session = self.conn.open_session()
        truncate_session.begin_transaction()
        cursor_start = truncate_session.open_cursor(uri)
        cursor_start.set_key(nrows // 2)
        truncate_session.truncate(None, cursor_start, None, None)

        # The commit fails because the commit timestamp is less than the previous durable timestamp.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: truncate_session.commit_transaction('commit_timestamp=' + self.timestamp_str(truncate_ts)),
            '/unexpected timestamp usage/')
        cursor_start.close()
        truncate_session.close()
