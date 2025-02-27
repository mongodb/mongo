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

import wiredtiger
from error_info_util import error_info_util

# test_error_info04.py
#   Test error information scenarios when an application thread gets pulled into eviction when
#   committing or rolling back a transaction. The original error of the commit/rollback should
#   be returned and not be saved inside the get_last_error() function call.
class test_error_info04(error_info_util):
    uri = "table:test_error_info.wt"
    conn_config = "cache_max_wait_ms=1,eviction_dirty_target=1,eviction_dirty_trigger=2"

    def test_commit_transaction_skip_save(self):
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Start 100 transactions which should be enough to trigger application eviction when committed.
        sessions = []
        for i in range(100):
            temp_session = self.conn.open_session()
            cursor = temp_session.open_cursor(self.uri)
            temp_session.begin_transaction()
            cursor.set_key(str(i))
            cursor.set_value(str(i)*1024*500)
            cursor.insert()
            sessions.append(temp_session)

        # Configure the lowest cache max wait time so that application attempts eviction.
        self.conn.reconfigure('cache_max_wait_ms=2')
        # Commit all transactions large enough to trigger eviction app worker threads.
        for temp_session in sessions:
            self.assertEqual(temp_session.commit_transaction(), 0)
            self.assert_error_equal(0, wiredtiger.WT_NONE, "last API call was successful")

        self.session.checkpoint()

    def test_rollback_transaction_skip_save(self):
        # Create a basic table.
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Start 100 transactions which should be enough to trigger application eviction when rolled back.
        sessions = []
        for i in range(100):
            temp_session = self.conn.open_session()
            cursor = temp_session.open_cursor(self.uri)
            temp_session.begin_transaction()
            cursor.set_key(str(i))
            cursor.set_value(str(i)*1024*500)
            cursor.insert()
            sessions.append(temp_session)

        # Configure the lowest cache max wait time so that application attempts eviction.
        self.conn.reconfigure('cache_max_wait_ms=2')
        # Rollback all transactions large enough to trigger eviction app worker threads.
        for temp_session in sessions:
            self.assertEqual(temp_session.rollback_transaction(), 0)
            self.assert_error_equal(0, wiredtiger.WT_NONE, "last API call was successful")
