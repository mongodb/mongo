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

# Test that verifying a table that is still awaiting publication does not throw away its
# committed data. Such a table lives only in memory until a checkpoint publishes it, so there
# is nothing on disk to verify. Verify skips it and leaves the data intact.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema20(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    stable_uri = f'file:{test_name}.wt_stable'
    table_config = 'key_format=i,value_format=S'

    nrows = 10

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def create_unpublished_table_with_data(self):
        """Create a table awaiting publication and write committed rows into it."""
        # The table is created in epoch 10 and published at epoch 20. The stable epoch
        # stays at 10, so no checkpoint can publish the table and it keeps its data in
        # memory.
        self.set_stable_epoch(10)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)

        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cursor[i] = f'value{i}'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30 + i))
        cursor.close()

        # A table awaiting publication may only hold unstable data. Keep the stable timestamp
        # below the committed rows so the data stays unstable, and provide the stable timestamp
        # a precise checkpoint needs, including the one taken when the connection closes.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30) +
                                ',oldest_timestamp=' + self.timestamp_str(1))

    def create_empty_unpublished_table(self):
        """Create a table awaiting publication that never receives any data."""
        self.set_stable_epoch(10)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30) +
                                ',oldest_timestamp=' + self.timestamp_str(1))

    def check_rows(self):
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0, f'key {i} is missing')
            self.assertEqual(cursor.get_value(), f'value{i}')
        cursor.close()

    def test_verify_unpublished_table(self):
        """
        Verify a table that is still awaiting publication. Such a table has nothing on disk,
        so verify skips it and must leave the committed rows intact rather than discarding them.
        """
        self.create_unpublished_table_with_data()
        self.check_rows()

        self.session.verify(self.uri, None)

        self.check_rows()

    def test_verify_empty_unpublished_table(self):
        """
        Verify an unpublished table that never held any data. It is still awaiting publication,
        so verify skips it and completes without error.
        """
        self.create_empty_unpublished_table()

        self.session.verify(self.uri, None)

    def test_verify_unpublished_table_is_idempotent(self):
        """Repeated verifies of an unpublished table keep skipping and never lose the data."""
        self.create_unpublished_table_with_data()

        self.session.verify(self.uri, None)
        self.session.verify(self.uri, None)

        self.check_rows()

    def test_verify_unpublished_table_with_config(self):
        """A strict verify of an unpublished table is skipped like any other and preserves data."""
        self.create_unpublished_table_with_data()

        self.session.verify(self.uri, 'strict=true')

        self.check_rows()

    def test_verify_unpublished_stable_constituent(self):
        """
        Verify the stable constituent file directly rather than through the layered URI. It is
        awaiting publication, so verify skips it and the data survives.
        """
        self.create_unpublished_table_with_data()

        self.session.verify(self.stable_uri, None)

        self.check_rows()

    def test_verify_across_publish(self):
        """
        Verify is skipped while the table awaits publication, then runs for real once a checkpoint
        publishes it. The committed data survives the whole sequence.
        """
        self.create_unpublished_table_with_data()

        self.session.verify(self.uri, None)
        self.check_rows()

        self.set_stable_epoch(20)
        self.leader_checkpoint(50)

        self.session.verify(self.uri, None)
        self.check_rows()

    def test_verify_published_table(self):
        """A checkpoint publishes the table, after which verify is harmless."""
        self.create_unpublished_table_with_data()

        self.set_stable_epoch(20)
        self.leader_checkpoint(50)

        self.session.verify(self.uri, None)

        self.check_rows()
