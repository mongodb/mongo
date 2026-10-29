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

# Test that a published but empty table stays published across a restart.
#
# An empty stable table writes no root page, so its checkpoint cookie is empty and
# indistinguishable by size from a never-published table. After a restart the table
# must not be treated as awaiting publication again: doing so makes later writes look
# like stable data in an unpublished table and panics the next checkpoint with
# "stable data checkpointed for unpublished table". The published table is recorded in
# the shared metadata table, which is the durable signal that survives a restart.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema19(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def publish_empty_and_checkpoint(self):
        """Create and publish a table, then checkpoint it while it is still empty."""
        # Publishing requires a stable schema epoch set strictly below the publish epoch.
        self.set_stable_epoch(10)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        # Advance the stable epoch to cover the publish, then checkpoint to make it durable.
        self.set_stable_epoch(20)
        self.leader_checkpoint(1)
        # Even empty, the published table is recorded in the shared metadata table.
        self.assertTrue(self.uri_in_shared_metadata(self.conn, self.uri))

    def test_write_after_empty_published_restart(self):
        """
        Write to a published empty table after a restart. Before the fix this panicked
        at checkpoint with "stable data checkpointed for unpublished table", because the
        empty stable checkpoint made the already-published table look unpublished again.
        """
        self.publish_empty_and_checkpoint()

        # Restart, discarding local files so the table is rebuilt from shared storage.
        self.restart_without_local_files(step_up=True)
        self.set_stable_epoch(20)

        # Write a row and checkpoint. This must not be mistaken for stable data in an
        # unpublished table.
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        cursor.close()
        self.leader_checkpoint(50)

        # The data is durable and readable.
        cursor = self.session.open_cursor(self.uri)
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'value')
        cursor.close()
