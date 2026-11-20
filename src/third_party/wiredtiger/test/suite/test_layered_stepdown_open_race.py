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

import threading, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# test_layered_stepdown_open_race.py
#    A layered cursor read at a timestamp that starts on the leader and first opens its stable
#    constituent while a step-down is in flight completes under the follower role, and returns
#    what the leader returned at that timestamp.
@disagg_test_class
class test_layered_stepdown_open_race(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:test_layered_stepdown_open_race'
    nkeys = 100
    read_ts = 25

    def value(self, i, ts):
        return f'value{i}@{ts}'

    def test_open_races_step_down(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for ts in (10, 20, 30):
            self.session.begin_transaction()
            for i in range(self.nkeys):
                cursor[str(i)] = self.value(i, ts)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.session.checkpoint()

        # Read on the leader between two versions, so neither the oldest nor the newest value is
        # correct.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(self.read_ts))
        on_leader = [cursor[str(i)] for i in range(self.nkeys)]
        self.session.rollback_transaction()
        cursor.close()
        self.assertEqual(on_leader, [self.value(i, 20) for i in range(self.nkeys)])

        # Restart so the stable constituent's handle is closed and the next open is a fresh one.
        self.reopen_conn()

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(self.read_ts))

        # The step-down sleeps holding the schema lock before it publishes the follower role.
        self.conn.reconfigure('timing_stress_for_test=[disagg_role_transition]')
        step_down = threading.Thread(
            target=lambda: self.conn.reconfigure('disaggregated=(role="follower")'))
        step_down.start()
        self.assertStatGreaterSoon(stat.conn.disagg_step_down_in_progress, 0, timeout=60,
            msg='the step-down never started')

        # The search reads the leader role, then its stable open waits for the step-down.
        self.assertEqual(cursor['0'], on_leader[0])
        step_down.join()
        self.conn.reconfigure('timing_stress_for_test=[]')
        self.assertGreater(self.get_stat(stat.conn.layered_stable_live_open_refused), 0)
        self.assertEqual(self.get_stat(stat.conn.layered_curs_open_stable_stepdown_race), 1)

        # Write on the follower: a newer version of every key, and a new key.
        writer = self.conn.open_session()
        write_cursor = writer.open_cursor(self.uri)
        writer.begin_transaction()
        for i in range(self.nkeys):
            write_cursor[str(i)] = self.value(i, 50)
        write_cursor['new'] = 'new'
        writer.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        write_cursor.close()
        writer.close()

        # The rest of the transaction, now on the follower, reads what the leader read and none of
        # the follower's writes.
        self.assertEqual([cursor[str(i)] for i in range(self.nkeys)], on_leader)
        cursor.set_key('new')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        # A later read sees the follower's writes.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        self.assertEqual([cursor[str(i)] for i in range(self.nkeys)],
            [self.value(i, 50) for i in range(self.nkeys)])
        self.assertEqual(cursor['new'], 'new')
        self.session.rollback_transaction()
        cursor.close()
