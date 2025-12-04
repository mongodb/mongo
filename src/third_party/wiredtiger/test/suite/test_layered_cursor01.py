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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, Oplog
from wtscenario import make_scenarios

# test_layered_cursor01.py
# Test different variations of cursor operations
@disagg_test_class
class test_layered_cursor01(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    uri = 'layered:test_layered_cursor01'
    session_follow = None
    conn_follow = None
    oplog = None
    table_oplog_id = None

    # Configurable values
    ninserts = 100
    nupdates = 0
    nremoves = 0
    remove_offset = 0
    update_offset = 0
    nitems = 0
    posoplog = 0

    def position_search(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(self.oplog.gen_key(key))
        cursor.search()
        return cursor

    def position_search_near(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(self.oplog.gen_key(key))
        cursor.search_near()
        return cursor

    def position_next(self, session, key):
        cursor = session.open_cursor(self.uri)
        while True:
            self.assertNotEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
            if cursor.get_key() == self.oplog.gen_key(key):
                break
        return cursor

    def position_prev(self, session, key):
        cursor = session.open_cursor(self.uri)
        while True:
            self.assertNotEqual(cursor.prev(), wiredtiger.WT_NOTFOUND)
            if cursor.get_key() == self.oplog.gen_key(key):
                break
        return cursor

    disagg_storages = gen_disagg_storages('test_layered_cursor01', disagg_only = True)
    pos_op = [
        ('search', dict(pos_func=position_search)),
        ('search_near', dict(pos_func=position_search_near)),
        ('next', dict(pos_func=position_next)),
        ('prev', dict(pos_func=position_prev)),
    ]
    scenarios = make_scenarios(disagg_storages, pos_op)

    def setup_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')

    def create_table(self):
        table_config = "key_format=S,value_format=S"
        self.session.create(self.uri, table_config)
        self.session_follow.create(self.uri, table_config)
        self.table_oplog_id = self.oplog.add_uri(self.uri)

    def oplog_apply_traffic(self):
        # Create some oplog traffic, a mix of inserts and updates
        self.oplog.insert(self.table_oplog_id, self.ninserts)
        if (self.nupdates > 0):
            self.oplog.update(self.table_oplog_id, self.nupdates, self.nitems + self.update_offset)
        if (self.nremoves > 0):
            self.oplog.remove(self.table_oplog_id, self.nremoves, self.nitems + self.remove_offset)

        # Apply on both leader and follower
        nops = self.ninserts + self.nupdates + self.nremoves
        self.oplog.apply(self, self.session, self.posoplog, nops)
        self.oplog.apply(self, self.session_follow, self.posoplog, nops)

        # Update the expected number of items in the table
        self.nitems += self.ninserts
        self.posoplog += nops

    def check_key_value(self, cursor, exp_enty):
        expected_key = self.oplog.gen_key(exp_enty[0])
        expected_value = self.oplog.gen_value(exp_enty[1])

        self.assertEqual(expected_key, cursor.get_key())
        self.assertEqual(expected_value, cursor.get_value())

    def check_position(self, session, table, start_pos):
        pos_key = table[start_pos][0]

        # Check forward direction
        next_cursor = self.pos_func(self, session, pos_key)
        self.check_key_value(next_cursor, table[start_pos])

        # Iterate from the positioned entry to the end of the table
        for next_idx in range(start_pos + 1, len(table)):
            next_cursor.next()
            self.check_key_value(next_cursor, table[next_idx])

        # Make sure that calling next after getting to the last element returns NOTFOUND
        self.assertEqual(next_cursor.next(), wiredtiger.WT_NOTFOUND)
        next_cursor.close()

        prev_cursor = self.pos_func(self, session, pos_key)
        self.check_key_value(prev_cursor, table[start_pos])

        # Iterate from the positioned entry to the beginning of the table
        for prev_idx in range(start_pos - 1, -1, -1):
            prev_cursor.prev()
            self.check_key_value(prev_cursor, table[prev_idx])

        # Make sure that calling prev after getting to the first element returns NOTFOUND
        self.assertEqual(prev_cursor.prev(), wiredtiger.WT_NOTFOUND)
        prev_cursor.close()

    def scan_table(self, session, table):
        next_cursor = session.open_cursor(self.uri)
        for entry in table:
            next_cursor.next()
            self.check_key_value(next_cursor, entry)
        self.assertEqual(next_cursor.next(), wiredtiger.WT_NOTFOUND)
        next_cursor.close()

        prev_cursor = session.open_cursor(self.uri)
        for entry in reversed(table):
            prev_cursor.prev()
            self.check_key_value(prev_cursor, entry)
        self.assertEqual(prev_cursor.prev(), wiredtiger.WT_NOTFOUND)
        prev_cursor.close()

    def check_cursor_ops(self):
        # Get the table expected content and turn it to an array of (key, value) tuples
        table = self.oplog.get_table_snapshot(self.table_oplog_id)

        # Check on the beginning, 25%, 50%, 75%, 100%.
        positions_to_check = [0, len(table) // 4, len(table) // 2, (3 * len(table)) // 4]
        if len(table) > 0:
            positions_to_check.append(len(table) - 1)

        for session in [self.session, self.session_follow]:
            self.scan_table(session, table)

            if len(table) == 0:
                continue

            for start_pos in positions_to_check:
                self.check_position(session, table, start_pos)

    def checkpoint_and_advance(self, ts = None):
        if not ts:
            ts = self.oplog.last_timestamp()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')
        self.session.checkpoint()
        self.check_cursor_ops()

        self.disagg_advance_checkpoint(self.conn_follow)
        self.check_cursor_ops()

    def test_empty_tables(self):
        self.oplog = Oplog()
        self.setup_follower()

        self.pr('create tables')
        self.create_table()
        self.check_cursor_ops()

        self.pr('empty checkpoint')
        self.checkpoint_and_advance(ts = 1) # zero timestamp is not allowed, set it to 1

    def test_populated_tables(self):
        self.oplog = Oplog()
        self.setup_follower()

        self.pr('create tables')
        self.create_table()
        self.check_cursor_ops()

        self.pr('apply the first entries batch')
        self.oplog_apply_traffic()
        self.check_cursor_ops()

        self.pr('checkpoint the first batch changes')
        self.checkpoint_and_advance()
        self.check_cursor_ops()

        self.pr('apply the second entries batch')
        self.oplog_apply_traffic()
        self.check_cursor_ops()

        self.pr('checkpoint the second batch')
        self.checkpoint_and_advance()

    # Tests with updates
    def test_populated_tables_with_updates_20_percent(self):
        self.nupdates = (self.ninserts * 20) // 100
        self.test_populated_tables()

    def test_populated_tables_with_updates_50_percent(self):
        self.nupdates = (self.ninserts * 50) // 100
        self.test_populated_tables()

    def test_populated_tables_with_updates_70_percent(self):
        self.nupdates = (self.ninserts * 70) // 100
        self.test_populated_tables()

    # Tests with removes
    def test_populated_tables_with_removes_20_percent(self):
        self.nremoves = (self.ninserts * 20) // 100
        self.test_populated_tables()

    def test_populated_tables_with_removes_50_percent(self):
        self.nremoves = (self.ninserts * 50) // 100
        self.test_populated_tables()

    def test_populated_tables_with_removes_70_percent(self):
        self.nremoves = (self.ninserts * 70) // 100
        self.test_populated_tables()

    # Tests with both updates and removes
    def test_populated_tables_with_removes_20_updates_50_percent(self):
        self.nupdates = (self.ninserts * 50) // 100
        self.nremoves = (self.ninserts * 20) // 100
        self.test_populated_tables()

    # Tests with offsets
    def test_populated_tables_with_updates_20_percent(self):
        self.nupdates = (self.ninserts * 20) // 100
        self.updates_offset = (self.ninserts * 20) // 100
        self.test_populated_tables()

    def test_populated_tables_with_removes_20_percent_offset(self):
        self.nremoves = (self.ninserts * 20) // 100
        self.remove_offset = (self.ninserts * 20) // 100
        self.test_populated_tables()

    def test_populated_tables_with_removes_20_updates_20_percent_offset(self):
        self.nupdates = (self.ninserts * 20) // 100
        self.updates_offset = (self.ninserts * 20) // 100
        self.nremoves = (self.ninserts * 20) // 100
        self.remove_offset = (self.ninserts * 20) // 100
        self.test_populated_tables()
