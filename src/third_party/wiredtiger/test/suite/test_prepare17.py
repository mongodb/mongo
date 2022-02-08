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

import time
import wttest
from wtscenario import make_scenarios

# test_prepare17
class test_prepare17(wttest.WiredTigerTestCase):
    # Set eviction triggers to 39% of cache.
    trigger = 39
    target = 35

    conn_config = f'cache_size=1MB,' + \
        f'eviction_trigger={trigger}, eviction_target={target},' + \
        f'eviction_dirty_trigger={trigger}, eviction_dirty_target={target},' + \
        f'eviction_updates_trigger={trigger}, eviction_updates_target={target},'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('int-row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_prepare_cache_stuck_trigger(self):
        uri = 'table:cache_stuck_on_prepared_update'
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, config)

        cursor = self.session.open_cursor(uri, None)
        self.session.begin_transaction()

        # A 400KB update in 1MB of cache exceeds the 39% eviction threshold.
        cursor[1] = 'a' * 400*1024

        # Prepare the transaction, allowing for the updates to be evicted.
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(5))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(5))

        # Kill the cursor to make sure the updates aren't pinned and give the WT eviction
        # server some time to evict.
        cursor.close()
        time.sleep(1)

        # The prepared update resides on the disk, however, as part of the process to commit the prepared update, 
        # the page with the update is read back into the cache and the history page will be read in order to fix the stop 
        # timestamp on any existing update in history store.
        # Before the history page is read into the cache, it is checked whether we need to evict a page before bringing in the new page.
        # Since the update already exceeds the 39% eviction threshold, it is possible that a cache stuck may occur.
        # Hence, to avoid cache stuck we do not perform eviction checks(whether we need to evict a page) if the transaction is prepared.
        self.session.commit_transaction()
        self.session.close()
