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
#
# test_layered_schema33.py
#    Dropping a layered table whose create was never published dequeues the create, rather than
#    leaving a create and a remove stuck in the shared metadata queue forever.

import wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema33(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    table_config = 'key_format=S,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def layered_uri(self, name):
        """A distinct layered table URI."""
        return f'layered:{self.test_name}_{name}'

    def conn_stat(self, conn, stat_key):
        """Read a connection statistic from the given connection."""
        session = conn.open_session('')
        cursor = session.open_cursor('statistics:')
        val = cursor[stat_key][2]
        cursor.close()
        session.close()
        return val

    def assert_fully_absent(self, conn, uri):
        """The table has no stable constituent locally and nothing in shared metadata."""
        self.assertFalse(self.uri_stable_exists(conn, uri))
        self.assertFalse(self.uri_in_shared_metadata(conn, uri))

    def test_drop_unpublished_layered(self):
        """A drop of an unpublished table empties the queue and survives role transitions."""
        self.set_stable_epoch(1)
        uri = self.layered_uri('unpublished')
        self.session.create(uri, self.table_config)
        self.session.drop(uri)

        # Nothing is stuck in the queue: the checkpoint defers no entries.
        self.leader_checkpoint(10)
        self.assertEqual(
            self.conn_stat(self.conn, stat.conn.checkpoint_disagg_metadata_unstable), 0)
        self.assert_fully_absent(self.conn, uri)

        # Role transitions do not resurrect the dropped table.
        self.step_down()
        self.step_up()
        self.assert_fully_absent(self.conn, uri)

    def test_drop_unpublished_table_uri(self):
        """The same through the table URI, which queues two creates for one table."""
        self.set_stable_epoch(1)
        name = f'{self.test_name}_table_uri'
        self.session.create(f'table:{name}', self.table_config + ',type=layered')
        self.session.drop(f'table:{name}')

        self.leader_checkpoint(10)
        self.assertEqual(
            self.conn_stat(self.conn, stat.conn.checkpoint_disagg_metadata_unstable), 0)
        self.assert_fully_absent(self.conn, f'layered:{name}')

    def test_recreate_after_publish(self):
        """A published incarnation's queue entries survive a later unpublished create and drop."""
        self.set_stable_epoch(1)
        uri = self.layered_uri('recreated')

        # The first incarnation is published, so its drop must reach shared metadata.
        self.session.create(uri, self.table_config)
        self.publish(uri, 10)
        self.session.drop(uri)

        # The second incarnation is never published, so its drop dequeues its create.
        self.session.create(uri, self.table_config)
        self.session.drop(uri)

        # The checkpoint covers the published create, and the only entry it defers is the first
        # incarnation's still-unpublished remove: the second incarnation left nothing behind.
        self.set_stable_epoch(10)
        self.leader_checkpoint(10)
        self.assertTrue(self.uri_in_shared_metadata(self.conn, uri))
        self.assertEqual(
            self.conn_stat(self.conn, stat.conn.checkpoint_disagg_metadata_unstable), 1)

        # Publishing that remove lets the next checkpoint apply it, taking the table out of shared
        # metadata. The deferral counter stops growing because the queue is now empty.
        deferred = self.conn_stat(self.conn, stat.conn.checkpoint_disagg_metadata_unstable)
        self.publish(uri, 20)
        self.set_stable_epoch(20)
        self.leader_checkpoint(20)
        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))
        self.assertEqual(
            self.conn_stat(self.conn, stat.conn.checkpoint_disagg_metadata_unstable), deferred)

    def test_drop_unpublished_on_follower(self):
        """A follower dequeues too, so its step-up does not rebuild the dropped table."""
        self.set_stable_epoch(1)
        self.leader_checkpoint(5)
        conn_follow, session_follow = self.open_follower_epoch(1)

        uri = self.layered_uri('on_follower')
        session_follow.create(uri, self.table_config)
        session_follow.drop(uri)

        self.step_down()
        self.step_up(conn_follow)
        self.assert_fully_absent(conn_follow, uri)

        self.close_follower(conn_follow, session_follow)
