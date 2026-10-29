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

# With checkpoint deferral enabled, a follower must make incremental progress: a
# deferred checkpoint is adopted as soon as the transactions whose snapshots
# predate it finish, and a transaction that begins while a checkpoint is
# deferred covers that checkpoint, so its adoption never disturbs the newer
# transaction — even as further checkpoints keep arriving and being deferred.

import time
import wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_follower20(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
        # Deferral on: every adoption in this test comes from blocking
        # transactions finishing.
        return self.extensionsConfig() + self.conn_base_config + \
            'disaggregated=(role="follower")'

    def put(self, session, uri, kv, ts):
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for key, value in kv.items():
            cursor[key] = value
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def leader_checkpoint(self, ts):
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')
        self.session.checkpoint()

    def get_stat(self, conn, stat_id):
        session = conn.open_session('')
        cursor = session.open_cursor('statistics:')
        value = cursor[stat_id][2]
        cursor.close()
        session.close()
        return value

    def wait_for_stat(self, conn, stat_id, expected, timeout_sec=30):
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            if self.get_stat(conn, stat_id) >= expected:
                return
            time.sleep(0.1)
        self.fail(f'statistic did not reach {expected} within {timeout_sec}s')

    def read(self, session, key):
        # Read through a newly opened cursor, so the stable constituent binds now.
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        ret = cursor.search()
        value = cursor.get_value() if ret == 0 else None
        cursor.close()
        return (ret, value)

    def test_incremental_adoption(self):
        pickups = stat.conn.layered_table_manager_checkpoints_disagg_pick_up_succeed
        defers = stat.conn.disagg_checkpoint_defer

        # Leader: baseline data sealed into a first checkpoint; the follower
        # replicates the writes into its ingest and adopts the checkpoint.
        self.session.create(self.uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key': 'v1'}, 10)
        self.leader_checkpoint(10)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        self.put(session_follow, self.uri, {'key': 'v1'}, 10)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.get_stat(conn_follow, pickups), 1)

        # Transaction A establishes its snapshot before the second checkpoint.
        session_a = conn_follow.open_session('')
        session_a.begin_transaction()
        self.assertEqual(self.read(session_a, 'key'), (0, 'v1'))

        # A second checkpoint arrives while A is active: it must be deferred.
        self.put(self.session, self.uri, {'key': 'v2'}, 20)
        self.leader_checkpoint(20)
        self.put(session_follow, self.uri, {'key': 'v2'}, 20)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertGreaterEqual(self.get_stat(conn_follow, defers), 1)
        self.assertEqual(self.get_stat(conn_follow, pickups), 1)

        # Transaction B begins while the checkpoint is deferred: its snapshot
        # covers it, so B must never be disturbed by its adoption.
        session_b = conn_follow.open_session('')
        session_b.begin_transaction()
        self.assertEqual(self.read(session_b, 'key'), (0, 'v2'))

        # A finishes: the deferred checkpoint is adopted even though B is
        # still active, and B keeps reading through freshly bound cursors.
        session_a.rollback_transaction()
        session_a.close()
        self.wait_for_stat(conn_follow, pickups, 2)
        self.assertEqual(self.read(session_b, 'key'), (0, 'v2'))

        # A third checkpoint arrives while B is active: deferred again, and B
        # is still served.
        self.put(self.session, self.uri, {'key': 'v3'}, 30)
        self.leader_checkpoint(30)
        self.put(session_follow, self.uri, {'key': 'v3'}, 30)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.get_stat(conn_follow, pickups), 2)
        self.assertEqual(self.read(session_b, 'key'), (0, 'v2'))

        # B finishes: the third checkpoint is adopted and fully visible.
        session_b.rollback_transaction()
        session_b.close()
        self.wait_for_stat(conn_follow, pickups, 3)

        session = conn_follow.open_session('')
        session.begin_transaction()
        self.assertEqual(self.read(session, 'key'), (0, 'v3'))
        session.rollback_transaction()
        session.close()

        session_follow.close()
        conn_follow.close()

    def test_partial_adoption(self):
        # Several checkpoints are deferred behind overlapping readers; when the
        # oldest reader finishes, the follower must adopt exactly up to the
        # newest checkpoint the remaining readers cover — no further — and
        # every reader must keep seeing exactly its snapshot's value.
        pickups = stat.conn.layered_table_manager_checkpoints_disagg_pick_up_succeed
        adopted = stat.conn.disagg_checkpoint_meta_lsn
        delivered = stat.conn.disagg_checkpoint_delivered_lsn

        self.session.create(self.uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key': 'v1'}, 10)
        self.leader_checkpoint(10)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        self.put(session_follow, self.uri, {'key': 'v1'}, 10)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.get_stat(conn_follow, pickups), 1)

        # Reader A: snapshot before any deferred checkpoint.
        session_a = conn_follow.open_session('')
        session_a.begin_transaction()
        self.assertEqual(self.read(session_a, 'key'), (0, 'v1'))

        # Second checkpoint arrives (deferred behind A); reader B begins after
        # its arrival, so B's snapshot covers it.
        self.put(self.session, self.uri, {'key': 'v2'}, 20)
        self.leader_checkpoint(20)
        self.put(session_follow, self.uri, {'key': 'v2'}, 20)
        self.disagg_advance_checkpoint(conn_follow)
        lsn2 = self.get_stat(conn_follow, delivered)
        session_b = conn_follow.open_session('')
        session_b.begin_transaction()
        self.assertEqual(self.read(session_b, 'key'), (0, 'v2'))

        # Third checkpoint arrives (deferred behind A and B); reader C begins
        # after its arrival.
        self.put(self.session, self.uri, {'key': 'v3'}, 30)
        self.leader_checkpoint(30)
        self.put(session_follow, self.uri, {'key': 'v3'}, 30)
        self.disagg_advance_checkpoint(conn_follow)
        lsn3 = self.get_stat(conn_follow, delivered)
        self.assertGreater(lsn3, lsn2)
        session_c = conn_follow.open_session('')
        session_c.begin_transaction()
        self.assertEqual(self.read(session_c, 'key'), (0, 'v3'))
        self.assertEqual(self.get_stat(conn_follow, pickups), 1)

        # A finishes: the follower must adopt the second checkpoint (B covers
        # it) but not the third (B predates it). All readers keep their exact
        # snapshot values through freshly bound cursors.
        session_a.rollback_transaction()
        session_a.close()
        self.wait_for_stat(conn_follow, pickups, 2)
        self.assertEqual(self.get_stat(conn_follow, adopted), lsn2,
            'adopted past the checkpoint the remaining readers cover')
        self.assertEqual(self.get_stat(conn_follow, delivered), lsn3,
            'the third checkpoint was not delivered, so nothing held it back')
        self.assertEqual(self.read(session_b, 'key'), (0, 'v2'))
        self.assertEqual(self.read(session_c, 'key'), (0, 'v3'))

        # B finishes: the third checkpoint is adopted; C is unaffected.
        session_b.rollback_transaction()
        session_b.close()
        self.wait_for_stat(conn_follow, pickups, 3)
        self.assertEqual(self.get_stat(conn_follow, adopted), lsn3)
        self.assertEqual(self.read(session_c, 'key'), (0, 'v3'))
        session_c.rollback_transaction()
        session_c.close()

        # The follower has fully caught up.
        session = conn_follow.open_session('')
        session.begin_transaction()
        self.assertEqual(self.read(session, 'key'), (0, 'v3'))
        session.rollback_transaction()
        session.close()

        session_follow.close()
        conn_follow.close()
