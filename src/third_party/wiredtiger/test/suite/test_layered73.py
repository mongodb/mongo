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
# test_layered73.py
#   Test layered cursor prepare conflict handling
#   - Verify cursor key state is preserved when WT_PREPARE_CONFLICT is returned
#   - Ensure retry loops work correctly after prepare conflicts
#   - Test search, search_near, next, and prev operations

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered73(wttest.WiredTigerTestCase):
    tablename = 'test_layered73'
    uri = 'layered:' + tablename

    resolve_scenarios = [
        ('commit', dict(commit = True)),
        ('rollback', dict(commit = False)),
    ]

    disagg_storages = gen_disagg_storages('test_layered73', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolve_scenarios)

    conn_base_config = 'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="follower")'

    def setup_table_with_data(self, keys):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for key in keys:
            cursor[key] = f"value_{key}"
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        cursor.close()

    def prepare_key_in_separate_session(self, key, value, prepare_ts=50):
        prepare_session = self.conn.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri)

        prepare_session.begin_transaction()
        prepare_cursor[key] = value
        prepare_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(1))

        return prepare_session, prepare_cursor

    def test_search_near_key_preserved_on_prepare_conflict(self):
        # Setup: keys 1, 3, 5 committed
        self.setup_table_with_data([1, 3, 5])

        # Prepare a transaction on key 2 (between 1 and 3)
        prepare_session, prepare_cursor = self.prepare_key_in_separate_session(2, "prepared_value")

        # Open cursor and set key to search for prepared key
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        cursor.set_key(2)
        # Assert that search_near should return a prepare conflict and the cursor position doesn't reset
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.search_near())
        retrieved_key = cursor.get_key()
        self.assertEqual(retrieved_key, 2,
                "Key should be preserved after WT_PREPARE_CONFLICT")

        if self.commit:
            prepare_session.breakpoint()
            prepare_session.commit_transaction('commit_timestamp=' + self.timestamp_str(60)+',durable_timestamp='+self.timestamp_str(60))
            # Key 2 is now committed so calling search_near() again should return the committed value
            self.assertEqual(cursor.search_near(), 0)
            self.assertEqual(cursor.get_key(), 2)
            self.assertEqual(cursor.get_value(), "prepared_value")
        else:
            prepare_session.rollback_transaction()
        prepare_cursor.close()

    def test_next_key_preserved_on_prepare_conflict(self):
        # Setup: keys 1, 3, 5 committed
        self.setup_table_with_data([1, 3, 5])

        # Prepare a transaction on key 2 (between 1 and 3)
        prepare_session, prepare_cursor = self.prepare_key_in_separate_session(2, "prepared_value")

        # Open cursor and position at key 1, then try to move next (should hit prepared key 2)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        retrieved_key = cursor.get_key()
        self.assertEqual(retrieved_key, 1, "Should be positioned at key 1")

        # next() should encounter the prepared key 2 and return prepare conflict
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next())
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.get_key(),  "/requires key be set/")

        # After prepare conflict, key state should be preserved
        # Calling prev() should return the previous key
        self.assertEqual(cursor.prev(), 0)
        retrieved_key = cursor.get_key()
        self.assertEqual(retrieved_key, 1,
                "Key should be preserved after WT_PREPARE_CONFLICT on next()")

        if self.commit:
            prepare_session.breakpoint()
            prepare_session.commit_transaction('commit_timestamp=' + self.timestamp_str(60)+',durable_timestamp='+self.timestamp_str(60))
            # Key 2 is now committed so calling next() should return key 2
            self.assertEqual(cursor.next(), 0)
            self.assertEqual(cursor.get_key(), 2)
            self.assertEqual(cursor.get_value(), "prepared_value")
        else:
            prepare_session.rollback_transaction()

        prepare_cursor.close()

    def test_prev_key_preserved_on_prepare_conflict(self):
        # Setup: keys 1, 3, 5 committed
        self.setup_table_with_data([1, 3, 5])

        # Prepare a transaction on key 4 (between 3 and 5)
        prepare_session, prepare_cursor = self.prepare_key_in_separate_session(4, "prepared_value")

        # Open cursor and position at key 5, then try to move prev (should hit prepared key 4)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(60))

        cursor.set_key(5)
        self.assertEqual(cursor.search(), 0)
        retrieved_key = cursor.get_key()
        self.assertEqual(retrieved_key, 5, "Should be positioned at key 5")

        # prev() should encounter the prepared key 4
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.prev())
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.get_key(),  "/requires key be set/")

        # After prepare conflict, key state should be preserved
        # Calling next() should return the next key
        self.assertEqual(cursor.next(), 0)
        retrieved_key = cursor.get_key()
        self.assertEqual(retrieved_key, 5,
                "Key should be preserved after WT_PREPARE_CONFLICT on prev()")

        if self.commit:
            prepare_session.commit_transaction('commit_timestamp=' + self.timestamp_str(60)+',durable_timestamp='+self.timestamp_str(60))
            # Key 4 is now committed so calling prev() should return key 4
            self.assertEqual(cursor.prev(), 0)
            self.assertEqual(cursor.get_key(), 4)
            self.assertEqual(cursor.get_value(), "prepared_value")
        else:
            prepare_session.rollback_transaction()

        prepare_cursor.close()
