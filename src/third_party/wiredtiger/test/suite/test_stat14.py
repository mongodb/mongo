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

import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet, simple_key
from wtscenario import make_scenarios
from helper import WiredTigerStat

# test_stat14.py
# Check that eviction threshold stats are correctly updated.

class test_stat14(wttest.WiredTigerTestCase):
    uri = 'table:test_stat14'

    conn_config = 'statistics=(all)'

    def get_stat(self, stat_name, expect):
        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None, None)
        stat = stat_cursor[stat_name][2]
        self.assertEqual(stat, expect)
        stat_cursor.close()

    def test_eviction_threshold_stats(self):
        # Test that eviction threshold statistics are correctly reported and updated.
        # These stats are multiplied by 100 for precision (e.g., 80% becomes 8000).

        # Open a connection-level statistics cursor to check the threshold values
        stat_cursor = self.session.open_cursor('statistics:', None, None)

        # Check default values to ensure precision is working correctly.
        # The defaults include fractional percentages (e.g., 2.5% for updates_target)
        # which validates the *100 multiplication for two decimal places of precision.
        # Default values from config_def.c:
        # eviction_target=80 -> 8000
        # eviction_trigger=95 -> 9500
        # eviction_dirty_target=5 -> 500
        # eviction_dirty_trigger=20 -> 2000
        # eviction_updates_target=0 (auto-set to dirty_target/2 = 2.5) -> 250
        # eviction_updates_trigger=0 (auto-set to dirty_trigger/2 = 10) -> 1000
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_cache_full_target][2], 8000)
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_cache_full_trigger][2], 9500)
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_dirty_target][2], 500)
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_dirty_trigger][2], 2000)
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_updates_target][2], 250)
        self.assertEqual(stat_cursor[stat.conn.eviction_threshold_updates_trigger][2], 1000)
        stat_cursor.close()

        # Test with integer value: Reconfigure eviction_target to 70
        self.conn.reconfigure("eviction_target=70")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_cache_full_target][2], 7000)

        # Test with integer value: Reconfigure eviction_trigger to 85
        self.conn.reconfigure("eviction_trigger=85")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_cache_full_trigger][2], 8500)

        # Test with integer value: Reconfigure eviction_dirty_target to 10
        self.conn.reconfigure("eviction_dirty_target=10")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_dirty_target][2], 1000)

        # Test with integer value: Reconfigure eviction_dirty_trigger to 25
        self.conn.reconfigure("eviction_dirty_trigger=25")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_dirty_trigger][2], 2500)

        # Test with integer value: Reconfigure eviction_updates_target to 8
        self.conn.reconfigure("eviction_updates_target=8")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_updates_target][2], 800)

        # Test with integer value: Reconfigure eviction_updates_trigger to 15
        self.conn.reconfigure("eviction_updates_trigger=15")
        with WiredTigerStat(self.session) as stat_cursor:
            self.assertEqual(stat_cursor[stat.conn.eviction_threshold_updates_trigger][2], 1500)
