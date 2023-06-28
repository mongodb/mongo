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

# test_stat11.py
# Check for the presence of some stats, but does not actually check their values.


class test_stat11(wttest.WiredTigerTestCase):
    uri = 'table:test_stat11'
    conn_config = 'statistics=(all)'
    create_params = 'key_format=i,value_format=i'
    stats = ['cache_eviction_blocked_checkpoint', 'cache_eviction_blocked_hazard',
             'cache_eviction_blocked_internal_page_split',
             'cache_eviction_blocked_overflow_keys', 'cache_eviction_blocked_recently_modified',
             'cache_eviction_blocked_uncommitted_truncate']

    def test_stats_exist(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        for s in self.stats:
            v = stat_cursor[getattr(wiredtiger.stat.conn, s)][2]
            # Use the value just in case; we would have already failed if it did not exist.
            self.assertNotEqual(v, None)
        stat_cursor.close()


if __name__ == '__main__':
    wttest.run()
