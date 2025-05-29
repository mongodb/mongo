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

import wiredtiger
import wttest
import time

# test_stat12.py
# Check for the presence of eviction-related statistics.
# Triggers the conditions and verifies the counters increment

class test_stat12(wttest.WiredTigerTestCase):
    uri = 'table:test_stat12'
    # conn_config = 'statistics=(all),cache_size=1MB'
    create_params = 'key_format=i,value_format=S'

    def conn_config(self):
        # Small cache to force eviction, enable all statistics
        return 'cache_size=1MB,statistics=(all),eviction=(threads_max=1),eviction_dirty_trigger=8,eviction_dirty_target=7,eviction_updates_trigger=5,eviction_updates_target=4,eviction_trigger=20,eviction_target=15'

    def populate_data(self, start, end, value_size=100):
        c = self.session.open_cursor(self.uri)
        for i in range(start, end):
            c[i] = 'x' * value_size
        c.close()

    def read_key(self, key):
        c = self.session.open_cursor(self.uri)
        c.set_key(key)
        self.assertEqual(c.search(), 0)
        c.close()

    def get_stat(self, stat_key):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_key][2]
        stat_cursor.close()
        return value

    def test_stats_eviction_trigger_exist(self):
        # Read the relevant eviction trigger stats
        clean_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_clean_reached)
        dirty_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_dirty_reached)
        updates_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_updates_reached)

        self.assertNotEqual(clean_trigger_count, None)
        self.assertNotEqual(dirty_trigger_count, None)
        self.assertNotEqual(updates_trigger_count, None)

    def test_stats_eviction_trigger_increments(self):
        self.session.create(self.uri, self.create_params)

        # Populate enough data to force eviction and thresholds
        self.populate_data(0, 10000, value_size=2000)  # Big values to fill cache
        self.session.checkpoint()

        # Additional reads to touch clean pages (increase memory usage) without dirtying
        for i in range(200, 10000):
            self.read_key(i)

        # Force dirty eviction
        for i in range(200, 3000):
            c = self.session.open_cursor(self.uri)
            c[i] = 'y' * 1000
            c.close()

        for _ in range(10):
            # Read the relevant eviction trigger stats
            clean_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_clean_reached)
            dirty_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_dirty_reached)
            updates_trigger_count = self.get_stat(wiredtiger.stat.conn.cache_eviction_trigger_updates_reached)
            if clean_trigger_count != 0 and dirty_trigger_count != 0 and updates_trigger_count != 0:
                break
            # Sleep to allow eviction to process
            time.sleep(1)

        # Print or assert their values have increased from 0
        self.assertGreaterEqual(clean_trigger_count, 1, "Clean trigger was not reached.")
        self.assertGreaterEqual(dirty_trigger_count, 1, "Dirty trigger was not reached.")
        self.assertGreaterEqual(updates_trigger_count, 1, "Updates trigger was not reached.")

