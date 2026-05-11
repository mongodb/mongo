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
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered96.py
#
# Regression tests: when two transactions share an open layered cursor, the "alternate"
# constituent cursor (the one not selected as current on the previous call, left parked at the
# next position, according to the current implementation) must be re-searched under the new
# transaction's snapshot before its cached value is used; otherwise it silently returns a stale result.
#
# Two triggering conditions are tested:
#  - the read_timestamp changes between transactions
#  - a new write commits to the ingest table between transactions

@disagg_test_class
class test_layered96(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered96'

    disagg_storages = gen_disagg_storages('test_layered96', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')

    def follow_next(self, cursor, key, value, txn_config = '', explicit_txn = True):
        if explicit_txn:
            self.session_follow.begin_transaction(txn_config)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), key)
        self.assertEqual(cursor.get_value(), value)
        if explicit_txn:
            self.session_follow.commit_transaction()

    def follow_prev(self, cursor, key, value, txn_config = '', explicit_txn = True):
        if explicit_txn:
            self.session_follow.begin_transaction(txn_config)
        self.assertEqual(cursor.prev(), 0)
        self.assertEqual(cursor.get_key(), key)
        self.assertEqual(cursor.get_value(), value)
        if explicit_txn:
            self.session_follow.commit_transaction()

    # --------------------------------------------------------------------------
    # Scenario 1: next(), stable is alternate, read_timestamp drops.
    #
    #   Stable (checkpoint)       Ingest (follower)
    #   +-----+----+-------+      +-----+----+-------+
    #   | key | ts | value |      | key | ts | value |
    #   +-----+----+-------+      +-----+----+-------+
    #   |   2 |  1 |   1   |      |   1 |  3 |   3   |
    #   |   2 |  2 |   2   |      +-----+----+-------+
    #   +-----+----+-------+
    #
    #   T1 (read_timestamp=3): next() -> key=1, value=3.
    #   T2 (read_timestamp=1): next() -> key=2, value=1.
    # --------------------------------------------------------------------------
    def test_table_scan_with_different_read_ts_stable_next(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: write key=2 at ts=1 then update at ts=2.
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c['2'] = '1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        c['2'] = '2'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()

        # Checkpoint and pick it up on the follower.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=1 sorts before key=2.
        cf = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cf['1'] = '3'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        cf.close()

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_next(cursor, '1', '3', 'read_timestamp=' + self.timestamp_str(3))
        self.follow_next(cursor, '2', '1', 'read_timestamp=' + self.timestamp_str(1)) # bug: returns '2'
        cursor.close()

    # --------------------------------------------------------------------------
    # Scenario 2: prev(), stable is alternate, read_timestamp drops.
    #
    #   Stable (checkpoint)       Ingest (follower)
    #   +-----+----+-------+      +-----+----+-------+
    #   | key | ts | value |      | key | ts | value |
    #   +-----+----+-------+      +-----+----+-------+
    #   |   1 |  1 |   1   |      |   2 |  3 |   3   |
    #   |   1 |  2 |   2   |      +-----+----+-------+
    #   +-----+----+-------+
    #
    #   T1 (read_timestamp=3): prev() -> key=2, value=3.
    #   T2 (read_timestamp=1): prev() -> key=1, value=1.
    # --------------------------------------------------------------------------
    def test_table_scan_with_different_read_ts_stable_prev(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: write key=1 at ts=1 then update at ts=2.
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c['1'] = '1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        c['1'] = '2'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()

        # Checkpoint and pick it up on the follower.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=2 sorts after key=1.
        cf = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cf['2'] = '3'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        cf.close()

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_prev(cursor, '2', '3', 'read_timestamp=' + self.timestamp_str(3))
        self.follow_prev(cursor, '1', '1', 'read_timestamp=' + self.timestamp_str(1)) # bug: returns '2'
        cursor.close()

    # --------------------------------------------------------------------------
    # Scenario 3: next(), ingest is alternate, read_timestamp increases.
    #
    #   Stable (checkpoint)       Ingest (follower)
    #   +-----+----+-------+      +-----+----+-------+
    #   | key | ts | value |      | key | ts | value |
    #   +-----+----+-------+      +-----+----+-------+
    #   |   1 |  1 |   1   |      |   2 |  2 |   2   |
    #   +-----+----+-------+      |   2 |  3 |   3   |
    #                             +-----+----+-------+
    #
    #   T1 (read_timestamp=2): next() -> key=1, value=1.
    #   T2 (read_timestamp=3): next() -> key=2, value=3.
    # --------------------------------------------------------------------------
    def test_table_scan_with_different_read_ts_ingest_next(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: write key=1 at ts=1.
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c['1'] = '1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        c.close()

        # Checkpoint and pick it up on the follower.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=2 at ts=2, then updated at ts=3.
        cf = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cf['2'] = '2'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        self.session_follow.begin_transaction()
        cf['2'] = '3'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        cf.close()

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_next(cursor, '1', '1', 'read_timestamp=' + self.timestamp_str(2))
        self.follow_next(cursor, '2', '3', 'read_timestamp=' + self.timestamp_str(3)) # bug: returns '2'
        cursor.close()

    # --------------------------------------------------------------------------
    # Scenario 4: prev(), ingest is alternate, read_timestamp increases.
    #
    #   Stable (checkpoint)       Ingest (follower)
    #   +-----+----+-------+      +-----+----+-------+
    #   | key | ts | value |      | key | ts | value |
    #   +-----+----+-------+      +-----+----+-------+
    #   |   2 |  1 |   1   |      |   1 |  2 |   2   |
    #   +-----+----+-------+      |   1 |  3 |   3   |
    #                             +-----+----+-------+
    #
    #   T1 (read_timestamp=2): prev() -> key=2, value=1.
    #   T2 (read_timestamp=3): prev() -> key=1, value=3.
    # --------------------------------------------------------------------------
    def test_table_scan_with_different_read_ts_ingest_prev(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: write key=2 at ts=1.
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c['2'] = '1'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        c.close()

        # Checkpoint and pick it up on the follower.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=1 at ts=2, then updated at ts=3.
        cf = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cf['1'] = '2'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        self.session_follow.begin_transaction()
        cf['1'] = '3'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        cf.close()

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_prev(cursor, '2', '1', 'read_timestamp=' + self.timestamp_str(2))
        self.follow_prev(cursor, '1', '3', 'read_timestamp=' + self.timestamp_str(3)) # bug: returns '2'
        cursor.close()

    # --------------------------------------------------------------------------
    # Scenarios 5/6: ingest is alternate, snapshot advances due to a new write between calls.
    # Parameterized by whether the first and second call use an explicit transaction.
    # --------------------------------------------------------------------------
    def snapshot_gen_ingest_next(self, first_explicit_txn, second_explicit_txn):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: key=1, value=1.
        c = self.session.open_cursor(self.uri)
        c['1'] = '1'
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=2, value=2 (visible before first call).
        cf = self.session_follow.open_cursor(self.uri)
        cf['2'] = '2'

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_next(cursor, '1', '1', explicit_txn=first_explicit_txn)

        # Update ingest key=2 to value=22 between calls.
        cf['2'] = '22'
        cf.close()

        if first_explicit_txn or second_explicit_txn:
            # Isolation boundary detected: ingest cursor re-searched, new value visible.
            self.follow_next(cursor, '2', '22', explicit_txn=second_explicit_txn)
        else:
            # Both autocommit: consistent scan, new write not visible.
            self.follow_next(cursor, '2', '2', explicit_txn=second_explicit_txn)
        cursor.close()

    def snapshot_gen_ingest_prev(self, first_explicit_txn, second_explicit_txn):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

        # Leader: key=2, value=2.
        c = self.session.open_cursor(self.uri)
        c['2'] = '2'
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # Follower ingest: key=1, value=1 (visible before first call).
        cf = self.session_follow.open_cursor(self.uri)
        cf['1'] = '1'

        cursor = self.session_follow.open_cursor(self.uri)
        self.follow_prev(cursor, '2', '2', explicit_txn=first_explicit_txn)

        # Update ingest key=1 to value=11 between calls.
        cf['1'] = '11'
        cf.close()

        if first_explicit_txn or second_explicit_txn:
            # Isolation boundary detected: ingest cursor re-searched, new value visible.
            self.follow_prev(cursor, '1', '11', explicit_txn=second_explicit_txn)
        else:
            # Both autocommit: consistent scan, new write not visible.
            self.follow_prev(cursor, '1', '1', explicit_txn=second_explicit_txn)
        cursor.close()

    def test_snapshot_gen_ingest_next_txn_txn(self):
        self.snapshot_gen_ingest_next(True, True)

    def test_snapshot_gen_ingest_next_txn_auto(self):
        self.snapshot_gen_ingest_next(True, False)

    def test_snapshot_gen_ingest_next_auto_txn(self):
        self.snapshot_gen_ingest_next(False, True)

    def test_snapshot_gen_ingest_next_auto_auto(self):
        self.snapshot_gen_ingest_next(False, False)

    def test_snapshot_gen_ingest_prev_txn_txn(self):
        self.snapshot_gen_ingest_prev(True, True)

    def test_snapshot_gen_ingest_prev_txn_auto(self):
        self.snapshot_gen_ingest_prev(True, False)

    def test_snapshot_gen_ingest_prev_auto_txn(self):
        self.snapshot_gen_ingest_prev(False, True)

    def test_snapshot_gen_ingest_prev_auto_auto(self):
        self.snapshot_gen_ingest_prev(False, False)
