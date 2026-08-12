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

# Ensure an idle table that awaits publication is not swept away, which would clear the flag that
# keeps it unpublished and leave its data unprotected.

import errno, time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema25(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,cache_cursors=false,' + \
        'file_manager=(close_scan_interval=1,close_idle_time=1,close_handle_minimum=0),'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    control_uri = f'table:{test_name}_control'
    table_config = 'key_format=i,value_format=S'
    nrows = 100
    sweep_timeout = 60

    def create_idle_tables(self):
        """Create the table and a control table, then leave both idle."""
        session = self.conn.open_session('')
        session.create(self.uri, self.table_config)
        self.publish(self.uri, 10, session=session)
        session.create(self.control_uri, self.table_config)
        session.close()

    def open_handles(self):
        """Return the dump of the handles the connection has open."""
        self.cleanStdout()
        self.conn.debug_info('handles')
        dump = self.readStdout(maxchars=500000)
        self.cleanStdout()
        return dump

    def sweep_idle_handles(self):
        """Wait for the sweep to close the idle control table, so it has walked every handle."""
        control_file = f'file:{self.test_name}_control.wt'
        self.assertTrue(control_file in self.open_handles(), 'the control table is not open')
        deadline = time.time() + self.sweep_timeout
        while control_file in self.open_handles():
            self.assertLess(time.time(), deadline,
                'sweep server did not close the idle control table in time')
            time.sleep(0.5)

    def write_rows(self, commit_ts):
        """Write nrows and commit at commit_ts."""
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = 'value'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def assert_awaits_publication(self):
        """
        Assert the table still awaits publication, which only a drop refused for unpublished data
        can show. A table that has lost the state is refused, if at all, for dirty data instead.
        """
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.uri))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertEqual(sub, wiredtiger.WT_DIRTY_DATA)
        self.assertTrue('unpublished data' in msg)

    def assert_all_rows(self, session):
        """Assert every written row is readable through the given session."""
        cursor = session.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            self.assertEqual(cursor.get_value(), 'value')
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)

    def setup_swept_connection(self):
        """Create the tables and let the sweep run over them."""
        # Precise checkpoint requires a stable timestamp at connection close.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)
        self.create_idle_tables()
        self.sweep_idle_handles()

    def test_sweep_keeps_table_awaiting_publication(self):
        # The table outlived the sweep, so its committed data is still protected.
        self.setup_swept_connection()
        self.write_rows(commit_ts=10)
        self.assert_awaits_publication()

        # Checkpoint below the schema epoch that publishes the table, with the rows above the
        # checkpoint timestamp so the checkpoint is legal. The table has to be left out.
        self.set_stable_epoch(6)
        self.leader_checkpoint(3)

        # A follower reading that checkpoint sees no table, rather than one missing rows.
        self.assert_awaits_publication()
        conn_follower, session_follower = self.open_follower()
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: session_follower.open_cursor(self.uri))
        self.close_follower(conn_follower, session_follower)

        # Publishing then delivers every row.
        self.set_stable_epoch(10)
        self.leader_checkpoint(11)
        conn_follower, session_follower = self.open_follower()
        self.assert_all_rows(session_follower)
        self.close_follower(conn_follower, session_follower)
