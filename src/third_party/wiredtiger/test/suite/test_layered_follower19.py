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

# A failure while a disaggregated follower adopts a checkpoint must leave the
# follower unchanged: no table may resolve to the new checkpoint, no metadata
# for a new table may appear, and retrying the same checkpoint later must
# succeed. The failure is injected with a fault-injection stress option.

import wiredtiger, wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_follower19(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    new_uri = f'layered:{test_name}_new'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
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

    def metadata(self, conn):
        # The whole local metadata, keyed by URI: the reference for what a
        # failed checkpoint adoption must restore.
        session = conn.open_session('')
        cursor = session.open_cursor('metadata:')
        entries = {k: v for k, v in cursor}
        cursor.close()
        session.close()
        return entries

    def read(self, conn, uri, key):
        session = conn.open_session('')
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        cursor.set_key(key)
        ret = cursor.search()
        value = cursor.get_value() if ret == 0 else None
        cursor.close()
        session.rollback_transaction()
        session.close()
        return (ret, value)

    def test_failed_pickup_rolls_back_cleanly(self):
        # Leader: baseline data sealed into a first checkpoint; the follower
        # replicates the writes into its ingest and adopts the checkpoint.
        self.session.create(self.uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, self.uri, {'key': 'old value'}, 10)
        self.leader_checkpoint(10)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        self.put(session_follow, self.uri, {'key': 'old value'}, 10)
        self.disagg_advance_checkpoint(conn_follow)

        # Leader: update the existing table and create a new one, then seal
        # both into a second checkpoint, so that adopting it must both update
        # existing metadata and insert metadata for a new table.
        self.put(self.session, self.uri, {'key': 'new value'}, 20)
        self.session.create(self.new_uri, self.table_config)
        self.put(self.session, self.new_uri, {'key2': 'value2'}, 20)
        self.leader_checkpoint(20)
        self.put(session_follow, self.uri, {'key': 'new value'}, 20)

        # Adopting the second checkpoint fails on the injected fault and is
        # reported as retryable. The local metadata before the attempt is the
        # reference for what the unroll must restore.
        metadata_before = self.metadata(conn_follow)
        conn_follow.reconfigure('timing_stress_for_test=[failpoint_disagg_checkpoint_apply]')
        try:
            self.disagg_advance_checkpoint(conn_follow)
            self.fail('checkpoint pickup unexpectedly succeeded with the failpoint enabled')
        except wiredtiger.WiredTigerError as e:
            self.assertTrue('busy' in str(e).lower(), str(e))
        self.ignoreStderrPatternIfExists('Failed to pick up')
        self.assertGreaterEqual(self.get_stat(conn_follow,
            stat.conn.layered_table_manager_checkpoints_disagg_pick_up_failed), 1)

        # The failed adoption must leave no trace. Comparing the whole local
        # metadata is what distinguishes a full unroll from a partial apply: a
        # checkpoint entry left behind for the existing table shows up here,
        # while the table's value reads the same either way.
        self.assertEqual(self.metadata(conn_follow), metadata_before,
            'the failed adoption left local metadata behind')
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session_follow.open_cursor(self.new_uri))
        self.assertEqual(self.read(conn_follow, self.uri, 'key'), (0, 'new value'))

        # Retrying the same checkpoint with the fault cleared succeeds and
        # makes both tables' checkpoint content visible.
        conn_follow.reconfigure('timing_stress_for_test=[]')
        self.disagg_advance_checkpoint(conn_follow)
        self.assertNotEqual(self.metadata(conn_follow), metadata_before,
            'the successful adoption did not update local metadata')
        self.assertEqual(self.read(conn_follow, self.uri, 'key'), (0, 'new value'))
        self.assertEqual(self.read(conn_follow, self.new_uri, 'key2'), (0, 'value2'))

        session_follow.close()
        conn_follow.close()
