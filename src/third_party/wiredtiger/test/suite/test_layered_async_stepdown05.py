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

# test_layered_async_stepdown05.py
#    Validation of the step-down timestamp itself and the timestamp guards it imposes.
@disagg_test_class
class test_layered_async_stepdown05(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # The cutoff cannot be replaced while one is set.
    def test_second_step_down_ts_rejected(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.assertEqual(self.step_down_ts_is_set(), 0)
        self.set_step_down_ts(20)
        self.assertEqual(self.step_down_ts_is_set(), 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(30), '/step down timestamp is already set/')

    # The cutoff is only valid on a leader.
    def test_step_down_ts_on_follower_rejected(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(20), '/can only be set on a disaggregated leader/')

    # The cutoff must sit at or ahead of all_durable; setting it exactly there is allowed.
    def test_step_down_ts_below_all_durable_rejected(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v'}, 10)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_step_down_ts(9),
            '/must not be older than the newest durable timestamp/')
        self.set_step_down_ts(10)

    # Stable may reach the cutoff exactly but never pass it.
    def test_stable_cannot_pass_cutoff(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.set_step_down_ts(20)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(21)),
            '/must not advance past the step down timestamp/')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

    # Setting the cutoff and advancing stable to it in one call takes full effect.
    def test_step_down_ts_and_stable_in_one_call(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20) +
                                ',step_down_timestamp=' + self.timestamp_str(20))

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(30), '/step down timestamp is already set/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25)),
            '/must not advance past the step down timestamp/')

        # Routing took effect from the same call.
        self.write_at(self.uri, {'k1': 'v'}, 30)
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'k1'})
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), set())

    # A cutoff below the current stable must be rejected: stable may never sit past it.
    def test_step_down_ts_below_stable_rejected(self):
        self.set_global_ts(1, 10)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.set_step_down_ts(5), '/must not be older than the stable timestamp/')

    # Transactions that begin after the cutoff is set and commit above it land in ingest.
    def test_commits_above_cutoff_succeed(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        for i, commit_ts in enumerate((30, 40, 50)):
            self.session.begin_transaction()
            cursor[f'post{i}'] = f'v{commit_ts}'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

        self.assertEqual(self.read_kvs_at(self.uri, 60),
            {'post0': 'v30', 'post1': 'v40', 'post2': 'v50'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 60),
            {'post0', 'post1', 'post2'})

    # Later commits at or below the cutoff are rejected.
    def test_commit_at_or_below_cutoff_rejected(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        for commit_ts in (15, 20):
            self.session.begin_transaction()
            cursor[f'k{commit_ts}'] = 'v'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.commit_transaction(
                    'commit_timestamp=' + self.timestamp_str(commit_ts)),
                '/must be after the step down timestamp/')
        cursor.close()

        # The rejected commits left nothing behind in either constituent.
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 25), set())
        self.assertEqual(self.read_kvs_at(self.uri, 25), {})

    # A commit with no timestamp succeeds and lands in ingest; this pins current behavior.
    def test_untimestamped_commit_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'v'
        self.session.commit_transaction()
        cursor.close()

        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 25), {'k1'})

    # The cutoff does not change all_durable behavior: an in-flight txn still clamps it, and it
    # moves normally once that txn resolves and later commits land.
    def test_step_down_ts_does_not_change_all_durable(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v'}, 10)

        # An in-flight transaction reserves commit timestamp 12; a later commit at 15 leaves a
        # hole, so all_durable is clamped just below the reservation.
        straddler = self.conn.open_session()
        scur = straddler.open_cursor(self.uri, None, None)
        straddler.begin_transaction()
        scur['held'] = 'v'
        straddler.timestamp_transaction('commit_timestamp=' + self.timestamp_str(12))
        self.write_at(self.uri, {'k2': 'v'}, 15)
        self.assertEqual(self.all_durable(), 11)

        self.set_step_down_ts(20)
        self.assertEqual(self.all_durable(), 11, 'setting the cutoff must not move all_durable')

        # The straddler resolves and the hole closes.
        straddler.rollback_transaction()
        scur.close()
        straddler.close()
        self.assertEqual(self.all_durable(), 15)

        # A commit above the cutoff carries all_durable past it: drained.
        self.write_at(self.uri, {'k3': 'v'}, 25)
        self.assertEqual(self.all_durable(), 25)
