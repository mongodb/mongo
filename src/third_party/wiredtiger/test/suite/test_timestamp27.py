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
#
# test_timestamp27.py
#   Timestamps: assert commit settings
#

import wiredtiger, wttest

# Test query-timestamp returns 0 if the timestamp is not set and set-timestamp of 0 fails.
class test_timestamp27_timestamp_notset(wttest.WiredTigerTestCase):
    def test_conn_query_notset(self):
        for ts in ['all_durable', 'last_checkpoint', 'oldest', 'oldest_reader',
            'oldest_timestamp', 'pinned', 'recovery', 'stable', 'stable_timestamp']:
                self.assertEquals(self.conn.query_timestamp('get=' + ts), "0")

    def test_session_query_notset(self):
        for ts in ['commit', 'first_commit', 'prepare', 'read']:
            self.assertEquals(self.session.query_timestamp('get=' + ts), "0")

    def test_conn_set_zero(self):
        for ts in ['durable', 'oldest', 'stable']:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.conn.set_timestamp(ts + '_timestamp=0'), '/zero not permitted/')

    def test_session_set_commit_zero(self):
        for ts in ['commit', 'durable']:
            self.session.begin_transaction()
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.commit_transaction(ts + '_timestamp=0'), '/zero not permitted/')

    def test_session_set_prepare_zero(self):
        for ts in ['prepare']:
            self.session.begin_transaction()
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.prepare_transaction('prepare_timestamp=0'), '/zero not permitted/')

    def test_session_set_timestamp_zero(self):
        self.session.begin_transaction()
        for ts in ['commit', 'durable', 'prepare', 'read']:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.timestamp_transaction(ts + '_timestamp=0'), '/zero not permitted/')

    def test_session_set_timestamp_uint_zero(self):
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.timestamp_transaction_uint(
            wiredtiger.WT_TS_TXN_TYPE_COMMIT, 0), '/zero not permitted/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.timestamp_transaction_uint(
            wiredtiger.WT_TS_TXN_TYPE_DURABLE, 0), '/zero not permitted/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.timestamp_transaction_uint(
            wiredtiger.WT_TS_TXN_TYPE_PREPARE, 0), '/zero not permitted/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.timestamp_transaction_uint(
            wiredtiger.WT_TS_TXN_TYPE_READ, 0), '/zero not permitted/')

if __name__ == '__main__':
    wttest.run()
