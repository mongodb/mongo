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
# test_layered_schema36.py
#    Creating a layered table, directly or through a table, queues one shared metadata operation.

import wiredtiger
import wttest
from wiredtiger import stat
from helper_disagg import DisaggSchemaEpochMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios


@disagg_test_class
class test_layered_schema36(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    """Creates of layered tables, directly or through a table."""

    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = (
        conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'
    )

    table_config = 'key_format=S,value_format=S,leaf_page_max=16KB'

    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True))

    def create_layered_tables(self, session):
        """Create a layered table directly and another through a table, and return their URIs."""
        session.create('layered:by_layered', self.table_config)
        session.create('table:by_table', self.table_config + ',type=layered')
        return ['layered:by_layered', 'layered:by_table']

    def test_create_queues_one_operation(self):
        """Each layered table create queues one operation."""
        self.set_stable_epoch(1)
        uris = self.create_layered_tables(self.session)

        self.leader_checkpoint(10)

        deferred = self.get_stat(stat.conn.checkpoint_disagg_metadata_unstable)
        self.assertEqual(deferred, len(uris))

    def test_step_up_creates_each_stable_table_once(self):
        """Step-up builds each missing stable table once, with its create configuration."""
        self.set_stable_epoch(1)
        self.leader_checkpoint(5)
        conn_follow, session_follow = self.open_follower_epoch(1)
        uris = self.create_layered_tables(session_follow)

        self.step_down()
        self.step_up(conn_follow)
        created = self.get_stat(
            stat.conn.disagg_step_up_missing_stable_tables_created, conn=conn_follow
        )
        configs = [self.stable_config(conn_follow, uri) for uri in uris]
        self.close_follower(conn_follow, session_follow)

        self.assertEqual(created, len(uris))
        for config in configs:
            self.assertIn('leaf_page_max=16KB', config)

    def test_create_shares_colgroup_without_data(self):
        """A table that is never written still reaches followers with its column group."""
        tables = {'typed': ',type=layered', 'explicit': ',source="layered:explicit"'}
        for name, layered_config in tables.items():
            self.session.create(f'table:{name}', self.table_config + layered_config)
            # Verify closes the table, so only its create can carry the column group to followers.
            self.session.verify(f'table:{name}')

        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        for name in tables:
            session_follow.open_cursor(f'table:{name}').close()
        self.close_follower(conn_follow, session_follow)

    def test_nonstandard_layered_tables_rejected(self):
        """A layered column group must be its table's only one and named after the table."""
        named_config = 'key_format=S,value_format=SS,columns=(k,v1,v2),colgroups=(c1)'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.session.create('table:named', named_config + ',type=layered'),
            '/a layered table cannot have named column groups/',
        )

        self.session.create('table:named', named_config)
        creates = {
            'colgroup:named:c1': 'columns=(v1,v2),type=layered',
            'table:sourced': self.table_config + ',type=layered,source="layered:other"',
        }
        for uri, config in creates.items():
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError,
                lambda: self.session.create(uri, config),
                '/named after its table and be its only column group/',
            )

        self.leader_checkpoint(10)

    def test_table_cannot_take_over_layered_table(self):
        """A table cannot be created over a layered table that already exists."""
        self.session.create('layered:existing', self.table_config)
        self.leader_checkpoint(10)

        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.session.create('table:existing', self.table_config + ',type=layered'),
            '/cannot create a table over the existing layered table/',
        )

    def test_repeated_column_group_create(self):
        """Repeating the create of a layered table's column group succeeds."""
        self.session.create('table:repeated', self.table_config + ',type=layered')
        self.session.create('colgroup:repeated', 'type=layered')

        self.leader_checkpoint(10)
