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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_txn32.py
# A follower must be able to read every table the leader made durable, including a
# table that the most recent checkpoints did not modify. The leader records a
# high-water mark of write generations that a follower adopts to recognize the
# leader's transaction ids as belonging to an earlier generation; if that mark did
# not cover an unmodified table, the follower would keep that table's foreign ids
# and fail to read its data.
@disagg_test_class
class test_txn32(wttest.WiredTigerTestCase):
    conn_base_config = 'create,statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    stable = 'layered:txn32_stable'
    churn = 'layered:txn32_churn'
    nrows = 50

    def check(self, session, uri, expected):
        c = session.open_cursor(uri)
        for i in range(self.nrows):
            c.set_key('k%d' % i)
            self.assertEqual(c.search(), 0)
            self.assertEqual(c.get_value(), expected)
        c.close()

    # Create the tables on both nodes, populate the stable table once and make it durable,
    # then churn a different table across many checkpoints so write generations climb well
    # past the untouched table's.
    def populate_and_churn(self, sfollow):
        self.session.create(self.stable, 'key_format=S,value_format=S')
        sfollow.create(self.stable, 'key_format=S,value_format=S')
        self.session.create(self.churn, 'key_format=i,value_format=S')
        sfollow.create(self.churn, 'key_format=i,value_format=S')

        c = self.session.open_cursor(self.stable)
        for i in range(self.nrows):
            c['k%d' % i] = 'value'
        c.close()
        self.session.checkpoint()

        for round in range(10):
            cc = self.session.open_cursor(self.churn)
            cc[round] = 'churn'
            cc.close()
            self.session.checkpoint()

    def test_follower_reads_table_untouched_by_latest_checkpoint(self):
        conn_follow = self.wiredtiger_open(
            'follower', self.extensionsConfig() + ',' + self.conn_config_follower)
        sfollow = conn_follow.open_session('')

        self.populate_and_churn(sfollow)

        # The follower adopts the latest checkpoint and must read the untouched table in
        # full, served from its stable component.
        self.disagg_advance_checkpoint(conn_follow)
        self.check(sfollow, self.stable, 'value')

        sfollow.close()
        conn_follow.close()

    def test_primary_reopen_reads_its_data(self):
        self.session.create(self.stable, 'key_format=S,value_format=S')
        c = self.session.open_cursor(self.stable)
        for i in range(self.nrows):
            c['k%d' % i] = 'value'
        c.close()
        self.session.checkpoint()

        # Churn a different table so write generations climb before the reopen.
        self.session.create(self.churn, 'key_format=i,value_format=S')
        for round in range(10):
            cc = self.session.open_cursor(self.churn)
            cc[round] = 'churn'
            cc.close()
            self.session.checkpoint()

        # Reopen directly as primary. The pick-up on reopen must re-establish the base write
        # generation; otherwise the previous run's committed data would not be recognized this
        # run and would be unreadable.
        self.reopen_disagg_conn(self.conn_config + ',')
        self.check(self.session, self.stable, 'value')

    def test_stepped_up_leader_reads_table_untouched_by_latest_checkpoint(self):
        conn_follow = self.wiredtiger_open(
            'follower', self.extensionsConfig() + ',' + self.conn_config_follower)
        sfollow = conn_follow.open_session('')

        self.populate_and_churn(sfollow)

        # Promote the follower to leader; the switch picks up the latest checkpoint. Its
        # step-up establishes the base write generation by scanning all files, before it
        # writes anything as leader.
        self.disagg_switch_follower_and_leader(conn_follow, self.conn)

        # The new leader must read the untouched table in full, and a checkpoint it takes
        # (using the base write generation established at step-up) must keep it readable.
        self.check(sfollow, self.stable, 'value')
        cc = sfollow.open_cursor(self.churn)
        cc[999] = 'churn'
        cc.close()
        sfollow.checkpoint()
        self.check(sfollow, self.stable, 'value')

        sfollow.close()
        conn_follow.close()
