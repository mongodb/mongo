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

# Dropping a layered table must tolerate its stable data having already been
# reclaimed by the page server. [WT-18101, fixed by WT-17996]
#
# Trimming a layered table's stable pages before the drop becomes durable is by
# design: it prevents leaking the table's data if the leader crashes after the
# drop is durable but before the trim runs. After restarting from the newest
# checkpoint, two-phase drop guarantees that replay of a dropped table only ever
# reissues drop(), so drop must handle pages the page server has already
# discarded rather than aborting on the missing root-page read.
#
# The first drop completes (trim is a no-op in palite); closing the connection
# without a final checkpoint rolls the local drop back, so the table is restored
# from the shared checkpoint on reopen. Palite's trim_table is a no-op, so this
# test stands in for a page server that has already reclaimed the pages: it
# deletes the stable constituent's rows from palite's pages table while the
# connection is closed, then reopens and replays drop(). Pre-fix the replayed
# drop aborts on the missing root page; the fix makes it tolerate it.

import re
import wttest
from helper_disagg import (
    disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin, DisaggCorruptionMixin)
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_drop02(
  wttest.WiredTigerTestCase, DisaggSchemaEpochMixin, DisaggCorruptionMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'

    uri = 'layered:drop02'
    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def stable_btree_id(self):
        cursor = self.session.open_cursor('metadata:')
        cursor.set_key(self.stable_uri(self.uri))
        self.assertEqual(cursor.search(), 0)
        value = cursor.get_value()
        cursor.close()
        return int(re.search(r'id=(\d+)', value).group(1))

    def setup_table_and_checkpoint(self):
        """Create, publish, and checkpoint the layered table; return its stable btree id."""
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))

        self.set_stable_epoch(1)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 2)
        self.set_stable_epoch(2)
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'stable_value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()
        self.leader_checkpoint(2)
        return self.stable_btree_id()

    def replay_drop(self, home='.'):
        """Open (or reopen) the home and replay the drop."""
        self.ignoreStdoutPattern('WT_VERB_METADATA')
        self.open_conn(home)
        self.session.drop(self.uri)

    def test_replayed_drop_after_close_conn(self):
        """Replay a drop on a table whose stable pages were reclaimed; it must not abort."""
        btree_id = self.setup_table_and_checkpoint()

        # The first drop completes (trim is a no-op in palite). Closing without a final checkpoint
        # rolls the local drop back, so on reopen the table is restored from the shared checkpoint.
        self.session.drop(self.uri)
        self.close_conn('debug=(skip_checkpoint=true)')

        # Stand in for a page server that has already reclaimed the table's pages.
        self.delete_all_table_pages(btree_id)

        # Reopen and replay the drop; it must tolerate the reclaimed root page rather than abort.
        self.replay_drop()

        # The closing checkpoint is precise and so needs a stable timestamp newer than the last
        # checkpoint's; the reopened connection has none set. Set one.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(3) + ',oldest_timestamp=' + self.timestamp_str(1))
