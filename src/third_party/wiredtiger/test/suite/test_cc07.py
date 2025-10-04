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

from test_cc01 import test_cc_base
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc07.py
# Verify checkpoint cleanup removes the obsolete time window from the pages.
class test_cc07(test_cc_base):
    conn_config_common = 'cache_size=1GB,statistics=(all),statistics_log=(json,wait=1,on_close=true)'

    # These settings set a limit to the number of btrees/pages that can be cleaned up per btree per
    # checkpoint.
    conn_config_values = [
        ('no_btrees', dict(expected_cleanup=False, obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_btree_max=0]')),
        ('no_pages', dict(expected_cleanup=False, obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=0]')),
        ('50_pages', dict(expected_cleanup=True, obsolete_tw_max=50, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=50]')),
        ('100_pages', dict(expected_cleanup=True, obsolete_tw_max=100, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=100]')),
        ('500_pages', dict(expected_cleanup=True, obsolete_tw_max=500, conn_config=f'{conn_config_common},heuristic_controls=[checkpoint_cleanup_obsolete_tw_pages_dirty_max=500]')),
    ]

    scenarios = make_scenarios(conn_config_values)

    def test_cc07(self):
        if self.runningHook('tiered'):
            self.skipTest("checkpoint cleanup cannot remove obsolete pages from tiered tables")

        create_params = 'key_format=i,value_format=S'
        nrows = 1000
        uri = 'table:cc07'
        value = 'k' * 1024

        self.session.create(uri, create_params)

        for i in range(10):
            # Append some data.
            self.populate(uri, nrows * (i), nrows * (i + 1), value)

            # Checkpoint.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows * (i + 1)))
            self.session.checkpoint()
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(nrows * (i + 1)))

        # Trigger checkpoint cleanup and wait for it to make progress.
        self.wait_for_cc_to_run()

        # Retrieve the number of pages we have cleaned up so far.
        btree_cc_stat = self.get_stat(stat.dsrc.checkpoint_cleanup_pages_obsolete_tw, uri)
        if self.expected_cleanup:
            assert btree_cc_stat <= self.obsolete_tw_max, f"Unexpected number of pages with obsolete tw cleaned: {btree_cc_stat} (max {self.obsolete_tw_max})"
        else:
            self.assertEqual(btree_cc_stat, 0)

        # Verify the connection-level stat.
        conn_cc_stat = self.get_stat(stat.conn.checkpoint_cleanup_pages_obsolete_tw)
        if self.expected_cleanup:
            self.assertGreater(conn_cc_stat, 0)
        else:
            self.assertEqual(conn_cc_stat, 0)

