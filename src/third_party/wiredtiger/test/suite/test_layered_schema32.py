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

# Recreating a dropped layered table allocates a new table that owns its own page log. A
# read cached across the drop must not bind the new table to the dropped table's page log:
# it would write its pages there, then fail to find them when it reopens.

import glob, json, os, subprocess
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema32(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S'
    nrows = 200

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def write_rows(self, commit_ts, value):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = value
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def read_all_rows(self):
        """Read the whole table and close the cursor, leaving the read cached."""
        cursor = self.session.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            count += 1
        cursor.close()
        return count

    def create_on_follower_then_step_up(self, epoch, commit_ts, value):
        """Create and populate the table as a follower, then step up to build the stable."""
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, epoch)
        self.write_rows(commit_ts, value)
        self.step_up()

    def pages_by_table(self):
        """Return {page log table id: page count} straight from the page log."""
        sqlite_exe = os.path.join(os.environ.get('WT_BUILDDIR', '.'), 'sqlite3')
        counts = {}
        home = os.path.abspath(self.home)
        for db in sorted(glob.glob(os.path.join(home, 'kv_home', 'pages_*.db'))):
            result = subprocess.run(
                [sqlite_exe, '-json', db,
                 'SELECT table_id, COUNT(*) AS n FROM pages GROUP BY table_id;'],
                capture_output=True, text=True)
            if result.returncode != 0 or not result.stdout.strip():
                continue
            for row in json.loads(result.stdout):
                counts[int(row['table_id'])] = counts.get(int(row['table_id']), 0) + int(row['n'])
        return counts

    def assert_owns_pages(self, table_id, label):
        """Assert the given table wrote pages under its own page log table."""
        counts = self.pages_by_table()
        self.assertGreater(counts.get(table_id, 0), 0,
            f'{label} (id {table_id}) owns no pages, page log holds {sorted(counts.items())}')

    #
    # Test cases
    #

    def test_recreate_after_cached_follower_read(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(1)

        # Step up to build the original table, then checkpoint it so it owns pages.
        self.step_down()
        self.create_on_follower_then_step_up(epoch=5, commit_ts=10, value='first')
        self.set_stable_epoch(6)
        self.leader_checkpoint(10)
        first_id = self.stable_id(self.conn, self.uri)
        self.assert_owns_pages(first_id, 'the original table')

        # Read the table as a follower, then drop it with that read still cached.
        self.step_down()
        self.assertEqual(self.read_all_rows(), self.nrows)
        self.session.drop(self.uri)

        # The recreated table must own its own page log table, not the dropped one's.
        self.create_on_follower_then_step_up(epoch=15, commit_ts=30, value='second')
        second_id = self.stable_id(self.conn, self.uri)
        self.assertNotEqual(second_id, first_id)
        self.set_stable_epoch(16)
        self.leader_checkpoint(30)
        self.assert_owns_pages(second_id, 'the recreated table')

        # A fresh connection reads back the recreated table's own content.
        conn_follower, session_follower = self.open_follower()
        cursor = session_follower.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            self.assertEqual(cursor.get_value(), 'second')
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)
        self.close_follower(conn_follower, session_follower)
