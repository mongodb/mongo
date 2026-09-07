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


@wttest.skip_for_hook("disagg", "Layout and eviction targets differ under disaggregated storage.")
@wttest.skip_for_hook("tiered", "Layout and eviction targets differ under tiered storage.")
class test_eviction06(wttest.WiredTigerTestCase):
    conn_config = (
        'cache_size=10MB,statistics=(all),eviction=(threads_min=1,threads_max=1),'
        'eviction_updates_trigger=20,eviction_updates_target=10,'
        'eviction_dirty_trigger=95,eviction_dirty_target=90')
    uri = 'table:eviction06'
    nrows = 20000

    def dominating_walks(self):
        return self.get_stat(wiredtiger.stat.conn.eviction_server_walk_dominating_cache)

    def test_walk_dominating_tree(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        value = 'a' * 1024
        for i in range(self.nrows):
            cursor[i] = value
        cursor.close()

        # Only count walks driven by this tree's own cache footprint.
        baseline = self.dominating_walks()

        # Keep updating in place so the tree dominates the cache in the updates dimension. The dirty
        # targets are set high so that eviction is working on updates rather than dirty content.
        # Sample the statistics as we go: once the eviction server is idle they stop advancing, so
        # waiting until after the workload would leave nothing to observe.
        for _ in range(10):
            for start in range(0, self.nrows, 1000):
                cursor = self.session.open_cursor(self.uri)
                self.session.begin_transaction()
                for i in range(start, start + 1000, 2):
                    cursor[i] = value
                self.session.commit_transaction()
                cursor.close()
            if self.dominating_walks() > baseline:
                break

        # The statistic is only incremented from inside the walk-period check, so growth here is
        # also evidence that the period was throttling the tree at the time.
        self.assertStatGreaterSoon(
            wiredtiger.stat.conn.eviction_server_walk_dominating_cache, baseline, timeout=5)


if __name__ == '__main__':
    wttest.run()
