#!/usr/bin/env python
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

import wttest
from test_cc01 import test_cc_base
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc09.py
# Verify checkpoint cleanup reads pages from the disk to remove any obsolete time window information
# present on the page.
@wttest.skip_for_hook("tiered", "Checkpoint cleanup does not support tiered tables")
class test_cc09(test_cc_base):
    conn_config_common = 'statistics=(all),statistics_log=(json,wait=1,on_close=true),verbose=(checkpoint_cleanup:0)'

    # These settings set a limit to the number of btrees/pages that can be cleaned up per btree per
    # checkpoint by the checkpoint cleanup thread.
    conn_config_values = [
        ('no_btrees', dict(expected_cleanup=False, cc_obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_btree_max=0]')),
        ('no_pages', dict(expected_cleanup=False, cc_obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=0]')),
        ('50_pages', dict(expected_cleanup=True, cc_obsolete_tw_max=50, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=50]')),
        ('100_pages', dict(expected_cleanup=True, cc_obsolete_tw_max=100, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=100]')),
        ('500_pages', dict(expected_cleanup=True, cc_obsolete_tw_max=500, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=500]')),
    ]

    # Necessary conditions for checkpoint cleanup to run.
    cc_scenarios = [
        ('newest_stop_durable_ts', dict(has_delete=True, bump_oldest_ts=False)),
        ('obsolete_ts', dict(has_delete=False, bump_oldest_ts=True)),
        ('none', dict(has_delete=False, bump_oldest_ts=False)),
    ]

    scenarios = make_scenarios(conn_config_values, cc_scenarios)

    def test_cc09(self):
        create_params = 'key_format=i,value_format=S'
        nrows = 100000
        uri = 'table:cc09'
        value = 'k' * 1024

        self.session.create(uri, create_params)
        self.populate(uri, 0, nrows, value)

        # Make all the inserted data stable and checkpoint to make everything clean.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows))
        self.session.checkpoint()

        # Bump the oldest timestamp to make some of the previously inserted data globally
        # visible. This makes any time window informaton associated with that data obsolete and
        # eligible for cleanup.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(2 * nrows // 3))

        # Restart to have everything on disk.
        self.reopen_conn()

        # Open the table and perform a read as we need the dhandle to be open for checkpoint cleanup
        # to process the table.
        cursor = self.session.open_cursor(uri, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.reset(), 0)

        if self.has_delete:
            self.session.begin_transaction()
            cursor.set_key(1)
            cursor.remove()
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(nrows + 1))
            self.session.checkpoint()

        if self.bump_oldest_ts:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(nrows))

        # Force checkpoint cleanup and wait for it to make progress. It should read pages from the
        # disk to clear the obsolete content if allowed to.
        self.wait_for_cc_to_run()

        cc_read_stat = self.get_stat(stat.conn.checkpoint_cleanup_pages_read_obsolete_tw)
        cc_dirty_stat = self.get_stat(stat.conn.checkpoint_cleanup_pages_obsolete_tw)

        # We may be expecting cleanup but we have to be in one of the valid scenarios for checkpoint
        # cleanup to do something.
        if self.expected_cleanup and (self.has_delete or self.bump_oldest_ts):
            assert cc_read_stat > 0, "Checkpoint cleanup did not read anything"
            assert cc_dirty_stat > 0, "Checkpoint cleanup did not dirty anything"
            assert cc_dirty_stat <= self.cc_obsolete_tw_max, f"Checkpoint cleanup dirtied too many pages {cc_dirty_stat} (max: {self.cc_obsolete_tw_max})"
        else:
            assert cc_read_stat == 0, f"Checkpoint cleanup is not expected to read anything ({cc_read_stat})"
            assert cc_dirty_stat == 0, f"Checkpoint cleanup is not expected to dirty anything ({cc_dirty_stat})"
