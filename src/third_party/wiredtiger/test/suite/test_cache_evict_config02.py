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
from wiredtiger import stat

# Test that cache eviction controls can be reconfigured dynamically
# and that WT_CACHE_EVICT_SCRUB_UNDER_TARGET behaves correctly.
class test_cache_evict_config02(wttest.WiredTigerTestCase):
    conn_config = "cache_size=5MB,statistics=(all),cache_eviction_controls=[scrub_evict_under_target_limit=false]"
    uri = "table:eviction02"

    def test_cache_eviction_reconfig_and_scrub(self):
        self.session.create(self.uri, "key_format=i,value_format=S")
        cursor = self.session.open_cursor(self.uri)

        # Repeatedly update 100 keys to fill cache usage with dirty / updates
        val = "x" * 5000
        for i in range(50000):
            cursor[i % 100] = val
        cursor.close()

        # Baseline eviction stats before enabling scrub
        stat_cursor = self.session.open_cursor("statistics:")
        pages_scrubbed_baseline = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()

        # Enable scrub_evict_under_target_limit flag
        self.conn.reconfigure(
            "cache_eviction_controls=[scrub_evict_under_target_limit=true]"
        )

        cursor = self.session.open_cursor(self.uri)
        for i in range(50000):
            cursor[i % 100] = val
        cursor.close()

        # Check eviction stats after enabling scrub
        stat_cursor = self.session.open_cursor("statistics:")
        pages_scrubbed_with_flag = stat_cursor[stat.conn.cache_write_restore][2]
        stat_cursor.close()

        # Check that by enabling scrub-under-target flag, more pages were scrub evicted
        self.assertGreater(
            pages_scrubbed_with_flag,
            pages_scrubbed_baseline,
            "Scrub eviction should increase restored updates when enabled"
        )
