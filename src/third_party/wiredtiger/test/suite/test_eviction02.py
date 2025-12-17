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

from wiredtiger import stat
from wtscenario import make_scenarios
import wttest

# test_eviction02.py
# Verify evicting a clean page removes any obsolete time window information
# present on the page.
class test_eviction02(wttest.WiredTigerTestCase):
    conn_config_common = 'cache_size=10MB,statistics=(all),statistics_log=(json,wait=1,on_close=true)'

    # These values set a limit to the number of pages that can be cleaned up per btree per
    # checkpoint.
    conn_config_values = [
        ('50', dict(obsolete_tw_max=50, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_pages_dirty=50]')),
        ('100', dict(obsolete_tw_max=100, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_pages_dirty=100]')),
        ('500', dict(obsolete_tw_max=500, conn_config=f'{conn_config_common},heuristic_controls=[obsolete_tw_pages_dirty=500]')),
    ]

    scenarios = make_scenarios(conn_config_values)

    def check_stat(self, prev_value, current_value):
        # Stats may have a stale value, allow some buffer.
        error_margin = 2
        threshold = self.obsolete_tw_max * error_margin
        diff = current_value - prev_value
        assert diff <= threshold, f"Unexpected number of pages with obsolete tw cleaned: {diff} (max {threshold})"

    def get_stat(self, stat, uri = ""):
        stat_cursor = self.session.open_cursor(f'statistics:{uri}')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def populate(self, uri, start_key, num_keys, value):
        c = self.session.open_cursor(uri, None)
        for k in range(start_key, num_keys):
            self.session.begin_transaction()
            c[k] = value
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(k + 1))
        c.close()

    def evict_cursor(self, uri, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (1, nrows + 1):
            evict_cursor.set_key(i)
            evict_cursor.search()
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def test_evict(self):
        create_params = 'key_format=i,value_format=S'
        nrows = 10000
        uri = 'table:eviction02'
        value = 'k' * 1024

        self.session.create(uri, create_params)

        prev_obsolete_tw_value = 0
        for i in range(10):
            # Append some data.
            self.populate(uri, nrows * i, nrows * (i + 1), value)

            # Checkpoint with cleanup.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nrows * (i + 1)))
            self.session.checkpoint()

            # Check statistics.
            current_obsolete_tw_value = self.get_stat(stat.dsrc.cache_eviction_dirty_obsolete_tw, uri)
            if i > 0:
                self.check_stat(prev_obsolete_tw_value, current_obsolete_tw_value)
            prev_obsolete_tw_value = current_obsolete_tw_value

            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(nrows * (i + 1)))

        self.session.checkpoint()
        self.evict_cursor(uri, nrows * 10)

        # Check statistics.
        current_obsolete_tw_value = self.get_stat(stat.dsrc.cache_eviction_dirty_obsolete_tw, uri)
        self.check_stat(prev_obsolete_tw_value, current_obsolete_tw_value)
        # Check also the connection level stat.
        self.assertGreater(self.get_stat(stat.conn.cache_eviction_dirty_obsolete_tw), 0)
