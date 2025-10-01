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

# For now, this is just making sure the flags are set without errors
# Test that cache eviction controls can be reconfigured dynamically
# and do not require a connection restart.
class test_cache_evict_config01(wttest.WiredTigerTestCase):
    conn_config = "cache_size=50MB,statistics=(all)"
    uri = "table:eviction01"

    def test_cache_eviction_reconfig(self):
        # Create a table and insert baseline data
        self.session.create(self.uri, "key_format=i,value_format=S")
        cursor = self.session.open_cursor(self.uri)
        for i in range(5):
            cursor[i] = "init" + str(i)
        cursor.close()

        # Try different eviction reconfigurations.
        configs = [
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=false,app_eviction_min_cache_fill_ratio=0,skip_update_obsolete_check=false]",
            "cache_eviction_controls=[incremental_app_eviction=true,scrub_evict_under_target_limit=true,app_eviction_min_cache_fill_ratio=10,skip_update_obsolete_check=true]",
            "cache_eviction_controls=[incremental_app_eviction=true,scrub_evict_under_target_limit=false,app_eviction_min_cache_fill_ratio=25,skip_update_obsolete_check=false]",
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=true,app_eviction_min_cache_fill_ratio=50,skip_update_obsolete_check=true]",
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=false,cache_tolerance_for_app_eviction=0,skip_update_obsolete_check=false]",
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=false,cache_tolerance_for_app_eviction=100,skip_update_obsolete_check=true]",
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=false,cache_tolerance_for_app_eviction=25,skip_update_obsolete_check=false]",
            "cache_eviction_controls=[incremental_app_eviction=false,scrub_evict_under_target_limit=false,cache_tolerance_for_app_eviction=20,skip_update_obsolete_check=false]",
        ]

        # Try different eviction failure reconfigurations.
        failure_configs = [
            "cache_eviction_controls=[app_eviction_min_cache_fill_ratio=-1]",
            "cache_eviction_controls=[app_eviction_min_cache_fill_ratio=60]",
            "cache_eviction_controls=[app_eviction_min_cache_fill_ratio=110]",
            "cache_eviction_controls=[cache_tolerance_for_app_eviction=-1]",
            "cache_eviction_controls=[cache_tolerance_for_app_eviction=110]",
        ]

        for cfg in configs:
            self.conn.reconfigure(cfg)

            # Verify the connection is still alive by inserting more rows
            cursor = self.session.open_cursor(self.uri)
            for i in range(5, 10):
                cursor[i] = "postreconfig_" + str(i)
            cursor.close()

            # Ensure rows can be read
            cursor = self.session.open_cursor(self.uri)
            count = sum(1 for _ in cursor)
            self.assertGreaterEqual(count, 5)
            cursor.close()

        for cfg in failure_configs:
             self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.conn.reconfigure(cfg), 'Invalid argument')

        self.ignoreStderrPatternIfExists('Invalid argument')
