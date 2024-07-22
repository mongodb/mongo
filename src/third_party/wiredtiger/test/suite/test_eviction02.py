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

from eviction_util import eviction_util
from wiredtiger import stat
from wtscenario import make_scenarios

# test_eviction02.py
# Verify evicting a clean page removes any obsolete time window information present on the page.
class test_eviction02(eviction_util):
    conn_config_common = 'cache_size=10MB,statistics=(all),statistics_log=(json,wait=1,on_close=true)'

    # These settings set a limit to the number of btrees/pages that can be cleaned up per btree per
    # checkpoint.
    conn_config_values = [
        ('no_btrees', dict(expected_cleanup=False, obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_btree_max=0]')),
        ('no_pages', dict(expected_cleanup=False, obsolete_tw_max=0, conn_config=f'{conn_config_common},heuristic_controls=[eviction_obsolete_tw_pages_dirty_max=0]')),
        ('50_pages', dict(expected_cleanup=True, obsolete_tw_max=50, conn_config=f'{conn_config_common},heuristic_controls=[eviction_obsolete_tw_pages_dirty_max=50]')),
        ('100_pages', dict(expected_cleanup=True, obsolete_tw_max=100, conn_config=f'{conn_config_common},heuristic_controls=[eviction_obsolete_tw_pages_dirty_max=100]')),
        ('500_pages', dict(expected_cleanup=True, obsolete_tw_max=500, conn_config=f'{conn_config_common},heuristic_controls=[eviction_obsolete_tw_pages_dirty_max=500]')),
    ]

    scenarios = make_scenarios(conn_config_values)

    def test_evict(self):
        create_params = 'key_format=i,value_format=S'
        nrows = 10000
        prev_obsolete_tw_value = 0
        # Stats may have a stale value, allow some buffer.
        threshold = self.obsolete_tw_max * 1.5
        uri = 'table:eviction02'
        value = 'k' * 1024

        self.session.create(uri, create_params)

        for i in range(10):
            # Add some data.
            start_key = nrows * i
            num_keys = nrows * (i + 1)
            self.populate(uri, start_key, num_keys, value)

            # Make all the inserted data stable and checkpoint to make everything clean.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(num_keys))
            self.session.checkpoint()

            # Bump the oldest timestamp to make some of the previously inserted data globally
            # visible. This makes any time window informaton associated with that data obsolete and
            # eligible for cleanup.
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(num_keys // 2))

            # Eviction should perform clean eviction here which triggers the assessement of obsolete
            # time window information.
            self.evict_cursor_tw_cleanup(uri, num_keys)

            # Retrieve the number of pages we have cleaned up so far.
            current_obsolete_tw_value = self.get_stat(stat.dsrc.cache_eviction_dirty_obsolete_tw, uri)
            if self.expected_cleanup:
                # The difference between two iterations should not exceed the maximum number of
                # pages configured as each iteration contains one checkpoint that resets the limit.
                # Note that we cannot assert after each iteration that we have cleaned pages as it
                # depends on eviction that may not always work.
                diff = current_obsolete_tw_value - prev_obsolete_tw_value
                prev_obsolete_tw_value = current_obsolete_tw_value
                assert diff <= threshold, f"Unexpected number of pages with obsolete tw cleaned: {diff} (max {threshold})"
            else:
                self.assertEqual(current_obsolete_tw_value, 0)

        # Verify the btree and connection-level stat. If we expect some cleanup, by the end of the
        # test, we must have done some work.
        btree_stat = self.get_stat(stat.dsrc.cache_eviction_dirty_obsolete_tw, uri)
        conn_stat = self.get_stat(stat.conn.cache_eviction_dirty_obsolete_tw)
        if self.expected_cleanup:
            self.assertGreater(btree_stat, 0)
            self.assertGreater(conn_stat, 0)
        else:
            self.assertEqual(btree_stat, 0)
            self.assertEqual(conn_stat, 0)
