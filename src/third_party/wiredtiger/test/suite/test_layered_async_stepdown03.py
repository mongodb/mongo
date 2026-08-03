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
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# test_layered_async_stepdown03.py
#    Straddler rollback guards: a writer that began before the step-down timestamp was set rolls
#    back.
@disagg_test_class
class test_layered_async_stepdown03(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # Straddler rolls back on write; retry after the step-down timestamp is set commits to ingest.
    def test_straddler_rollback_on_write(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Begin a transaction and write beforehand, so this write lands in stable.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['straddle'] = 'before'

        self.set_step_down_ts(20)

        # The next write by the straddling transaction must roll back.
        def straddle_write():
            cursor['straddle2'] = 'after'
        self.assert_step_down_rollback(straddle_write)
        self.session.rollback_transaction()
        cursor.close()

        # A retry after the timestamp is set routes cleanly to ingest.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['straddle'] = 'after'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'straddle'})
        self.assertEqual(self.read_keys_at(self.uri, 40), {'straddle'})

        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), set())

    # Straddler rollback applies to any write; remove rolls back like insert.
    def test_straddler_rollback_remove(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'base'}, 10)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor.set_key('k1')

        # The step-down timestamp is set while this transaction is in flight.
        self.set_step_down_ts(20)

        self.assert_step_down_rollback(lambda: cursor.remove())
        self.session.rollback_transaction()
        cursor.close()

    # A transaction that wrote beforehand rolls back at commit; the retry lands in ingest.
    def test_straddler_commit_rolls_back(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'

        self.set_step_down_ts(20)

        self.assert_step_down_rollback(
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30)))
        cursor.close()

        # The rolled-back write left nothing behind, in either constituent.
        self.assertEqual(self.read_kvs_at(self.uri, 40), {})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), set())
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), set())

        # The retry runs after the step-down timestamp is set and commits cleanly to ingest.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'v'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'k1'})

    # A straddler rolls back even when committing at or below the cutoff.
    def test_straddler_commit_below_cutoff_also_rolls_back(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'

        self.set_step_down_ts(20)

        self.assert_step_down_rollback(
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15)))
        cursor.close()

    # A straddler that wrote only a plain table still rolls back at commit.
    def test_straddler_plain_table_commit_rolls_back(self):
        plain_uri = f'table:{self.test_name}_plain'
        self.set_global_ts(1, 1)
        self.session.create(plain_uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(plain_uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'

        self.set_step_down_ts(20)

        self.assert_step_down_rollback(
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30)))
        cursor.close()

    # A read-only straddler commits normally; the guard only fires on writes.
    def test_readonly_straddler_commits_fine(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v'}, 10)

        # A read-only transaction begins before the step-down timestamp is set and commits after it.
        rcur = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        self.assertEqual(rcur['k1'], 'v')
        self.set_step_down_ts(20)
        self.assertEqual(rcur['k1'], 'v')
        # Committing a read-only transaction must succeed (no WT_ROLLBACK).
        self.session.commit_transaction()
        rcur.close()

    # Shared body: begin a stable write, set the cutoff, advance stable to it, checkpoint.
    def stable_writer_through_checkpoint(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'
        self.set_step_down_ts(20)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()
        return cursor

    # A write after the step-down checkpoint, with the step-down timestamp still set, rolls back.
    def test_stable_writer_write_after_checkpoint_rolls_back(self):
        cursor = self.stable_writer_through_checkpoint()

        def straddle_write():
            cursor['k2'] = 'v'
        self.assert_step_down_rollback(straddle_write)
        self.session.rollback_transaction()
        cursor.close()

    # A commit after the step-down checkpoint, with the step-down timestamp still set, rolls back.
    def test_stable_writer_commit_after_checkpoint_rolls_back(self):
        cursor = self.stable_writer_through_checkpoint()

        self.assert_step_down_rollback(
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30)))
        cursor.close()
