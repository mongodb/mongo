#!/usr/bin/env python3
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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# test_layered_eviction08.py
#    Content written before a step-down stays resident on the table the step-down makes read-only.
#    Eviction has to reach and discard it, or it is stranded and the cache fills up.
@disagg_test_class
class test_layered_eviction08(wttest.WiredTigerTestCase):
    # A low dirty target keeps eviction on dirty-only passes, which is where a read-only table is
    # skipped. The high updates target keeps those passes from also counting as update passes.
    conn_config = 'statistics=(all),precise_checkpoint=true,' \
                + 'cache_size=20MB,eviction_dirty_target=1,eviction_dirty_trigger=20,' \
                + 'eviction_updates_target=50,eviction_updates_trigger=60,' \
                + 'eviction=(threads_min=1,threads_max=1),' \
                + 'disaggregated=(role="follower",lose_all_my_data=true)'

    create_session_config = 'key_format=i,value_format=S,leaf_page_max=4KB,' \
                          + 'block_manager=disagg,log=(enabled=false)'

    uri = 'table:test_layered_eviction08'
    nrows = 1000
    value = 'v' * 1000
    batch = 250

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def write_rows(self, value, ts):
        """Write rows in batches, committing and advancing the timestamps as we go."""
        cursor = self.session.open_cursor(self.uri, None, None)
        for start in range(1, self.nrows + 1, self.batch):
            self.session.begin_transaction()
            for i in range(start, min(start + self.batch, self.nrows + 1)):
                cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
            ts += 1
        cursor.close()
        return ts

    def test_stranded_content_drains(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.create(self.uri, self.create_session_config)

        ts = self.write_rows(self.value, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts) +
            ',oldest_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

        # Leave stable behind this write so it cannot be written out before the step-down.
        self.write_rows(self.value.upper(), ts + 1)

        # A stranded page stays resident while anyone has its table open. Hold the table across the
        # step-down so the stranded pages are counted before eviction can discard any of them.
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.conn.reconfigure('disaggregated=(role="follower")')
        stranded = self.get_stat(stat.conn.cache_pages_dirty_stable)
        self.assertGreater(stranded, 0, 'the step-down stranded nothing')

        cursor.close()
        self.session.close()
        self.session = self.conn.open_session()

        # A follower never dirties or writes a stable page, so the count only drops when eviction
        # discards a stranded one. The only user table here, so eviction ends its passes parked on
        # it and has to discard from it anyway.
        deadline = time.time() + 120
        while self.get_stat(stat.conn.cache_pages_dirty_stable) >= stranded:
            self.assertLess(time.time(), deadline,
                'no page stranded by the step-down was ever discarded')
            time.sleep(0.5)
