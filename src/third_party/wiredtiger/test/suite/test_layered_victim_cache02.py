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

# A leader that checkpoints an update, steps down, then evicts must still
# serve the checkpointed value.

import time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_victim_cache02(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,' \
        + 'cache_cursors=false,cache_size=50MB,' \
        + 'eviction=(threads_min=1,threads_max=1),' \
        + 'file_manager=(close_idle_time=10000),' \
        + 'page_delta=(delta_pct=100,leaf_page_delta=true),' \
        + 'disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    disagg_config = 'victim_cache_max_entries=10000'

    create_session_config = 'key_format=S,value_format=S'
    table_name = test_name
    nitems = 1000
    old_value = 'A' * 64
    new_value = 'B' * 64

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, [
        ('shared', dict(prefix='table:',
            table_config='block_manager=disagg,log=(enabled=false),leaf_page_max=4KB')),
    ])

    def materialize(self):
        page_log = self.conn.get_page_log(self.vars.page_log)
        (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)
        page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, last_lsn)
        page_log.terminate(self.session)

    def evict_all(self, uri):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        for i in range(self.nitems):
            cursor.set_key(f'k{i:06d}')
            self.assertEqual(cursor.search(), 0)
            cursor.reset()
        self.session.rollback_transaction()
        cursor.close()

    def wait_for_victim_puts(self, puts_before):
        filler = 'file:cache_filler'
        session = self.conn.open_session()
        session.create(filler, 'key_format=Q,value_format=S')
        cursor = session.open_cursor(filler)
        deadline = time.time() + 30
        i = 0
        while self.get_stat(stat.conn.block_cache_puts) <= puts_before:
            self.assertLess(time.time(), deadline,
                'eviction did not put a page in the victim cache after step-down')
            cursor[i] = 'x' * 1024
            i += 1
            if i % 256 == 0:
                session.checkpoint()
                self.conn.reconfigure('cache_size=1MB')
        cursor.close()
        session.close()

    def test_stepdown_victim_cache_serves_checkpointed_value(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        uri = self.prefix + self.table_name
        self.session.create(uri, self.create_session_config + ',' + self.table_config)

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for i in range(self.nitems):
            cursor[f'k{i:06d}'] = self.old_value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.materialize()

        # Evict every leaf, then keep a cursor on another session so the page
        # stays resident across the update checkpoint and step-down.
        self.evict_all(uri)

        key = f'k{self.nitems // 2:06d}'
        pin_session = self.conn.open_session()
        pin = pin_session.open_cursor(uri, None, None)
        pin.set_key(key)
        self.assertEqual(pin.search(), 0)
        self.assertEqual(pin.get_value(), self.old_value)

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        cursor[key] = self.new_value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        scrub_before = self.get_stat(stat.conn.cache_scrub_restore)
        self.session.checkpoint()
        self.materialize()
        self.assertGreater(self.get_stat(stat.dsrc.rec_page_delta_leaf, uri), 0)
        self.assertEqual(self.get_stat(stat.conn.cache_scrub_restore), scrub_before)

        puts_before = self.get_stat(stat.conn.block_cache_puts)
        self.conn.reconfigure('disaggregated=(role="follower")')

        pin.close()
        pin_session.close()
        self.wait_for_victim_puts(puts_before)

        cursor = self.session.open_cursor(uri, None, None)
        self.assertEqual(cursor[key], self.new_value)
        cursor.close()
