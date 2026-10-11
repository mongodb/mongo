#!/usr/bin/env python3
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

# test_verify4.py
#    Opening a non-disaggregated connection with verify_metadata=true must verify the
#    history store.
class test_verify4(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    uri = 'table:verify_metadata_hs'

    def test_verify_metadata_checks_history_store(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Two committed versions of a key, so the older one moves into the history store.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor['a'] = 'v1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.begin_transaction()
        cursor['a'] = 'v2'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict_cursor.set_key('a')
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()
        self.session.checkpoint()

        # Reopen with verify_metadata=true. The history store holds a record for the table, so
        # verification must check the table against it.
        self.reopen_conn(config=self.conn_config + ',verify_metadata=true')
        self.assertStatGreaterSoon(stat.conn.session_hs_verify_btrees_checked, 0)
