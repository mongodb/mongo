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
import wiredtiger

# Test to reproduce and validate the fix for a bug in in-memory page eviction with aborted prepared updates.
# The bug occurred when an aborted prepared update at the tail of an update chain was incorrectly
# causing has_newer_updates to be set in __rec_upd_select_inmem, which then triggered an assertion
# failure in __split_multi_inmem.
@wttest.skip_for_hook("tiered", "In-memory tests do not support tiered storage")
class test_prepare44(wttest.WiredTigerTestCase):
    """Test that eviction of in-memory pages with aborted prepared updates
    does not trigger an assertion failure when preserve_prepared is enabled."""
    test_name = __qualname__

    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    uri = f'table:{test_name}'

    def test_evict_aborted_prepared_tail(self):
        create_config = 'key_format=i,value_format=S,in_memory=true,log=(enabled=false)'
        self.session.create(self.uri, create_config)

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        # Step 1: Create a prepared transaction on key 1, then rollback it.
        # With preserve_prepared=true, the aborted prepared update is retained
        # in the update chain with prepare_state=INPROGRESS and txnid=ABORTED.
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()
        cursor_prepare[1] = "prepared_value"
        session_prepare.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(10) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prepare.close()
        session_prepare.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(15))
        session_prepare.close()

        # Step 2: Insert a committed value for key 1. This update goes to the
        # head of the update chain, above the aborted prepared update.
        # Chain: HEAD [committed, ts=20] -> [aborted prepared INPROGRESS] -> NULL
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = "committed_value"
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(20))

        # Also insert some other keys to ensure the page has enough data.
        self.session.begin_transaction()
        for i in range(2, 1000):
            cursor[i] = "value_" + str(i)
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(20))

        # Do NOT advance oldest_timestamp past the committed updates' durable
        # timestamp. This keeps the committed updates not globally visible.

        # Step 3: Force eviction of the page containing key 1. This should not trigger an assertion failure and the aborted prepared update should be saved to disk image.
        session_evict = self.conn.open_session()
        session_evict.begin_transaction('ignore_prepare=true')
        evict_cursor = session_evict.open_cursor(self.uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(1)
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.get_value(), "committed_value")
        evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Step 4: Verify data integrity - the committed value should still be
        # readable after eviction.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), "committed_value")
        self.session.rollback_transaction()
        cursor.close()
