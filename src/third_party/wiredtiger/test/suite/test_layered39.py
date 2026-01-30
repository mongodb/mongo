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

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered39.py
# Ensure that we don't evict pages ahead of the materialization frontier
@disagg_test_class
class test_layered39(wttest.WiredTigerTestCase):
    conn_base_config = 'cache_size=75MB,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),transaction_sync=(enabled,method=fsync),' \
                     + 'disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    disagg_storages = gen_disagg_storages('test_layered39', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 200_000
    uri = 'layered:test_layered39'

    def session_create_config(self):
        return 'key_format=S,value_format=S,'

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def evict(self, uri, keys):
        self.session.begin_transaction()
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        for key in keys:
            evict_cursor.set_key(key)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

    def test_layered39(self):
        # Avoid checkpoint error with precise checkpoint
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        # The node started as a follower, so step it up as the leader
        self.conn.reconfigure('disaggregated=(role="leader")')
        page_log = self.conn.get_page_log(self.vars.page_log)

        # The cache is sized for this workload so that it results in a lot of eviction. Ensure that
        # we only evict pages that have been materialized.
        self.session.create(self.uri, self.session_create_config())
        cursor = self.session.open_cursor(self.uri, None, None)

        # Build key data structure for easier management and reuse in evict function
        data = []
        for i in range(self.nitems):
            keys = [
                "Hello " + f"{i}",
                "Hi " + f"{i}",
                "OK " + f"{i}"
            ]
            data.extend(keys)

        # Insert data using the prepared keys and corresponding values
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[data[i*3]] = "World"
            cursor[data[i*3+1]] = "There"
            cursor[data[i*3+2]] = "Go"
            self.session.commit_transaction('commit_timestamp='+self.timestamp_str(15))
            if i % 10_000 == 0:
                self.pr(f'Checkpoint {i}')
                self.session.checkpoint()
                (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
                self.pr(f"{i=} {last_lsn=}")
                self.assertEqual(ret, 0)
                page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
                # Test both ways of setting the last materialized LSN.
                if i % 20_000 == 0:
                    self.conn.reconfigure(f'disaggregated=(last_materialized_lsn={last_lsn})')
                else:
                    self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN,
                                               last_lsn)
        cursor.close()

        # Set stable timestamp to ensure all pages are written during checkpoint
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))

        self.session.checkpoint()

        (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        # Update the materialised lsn
        self.pr(f'Finalise the last materialised lsn = {last_lsn}')
        page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, last_lsn)

        # Evict pages to ensure all pages are written including the internal pages dirtied by the eviction of the split child pages
        self.evict(self.uri, data)

        self.session.checkpoint()
        (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        # Update the last materialised lsn
        self.pr(f'Finalise the last materialised lsn = {last_lsn}')
        page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, last_lsn)

        page_log.terminate(self.session) # dereference

        self.pr(f'cache_scrub_restore = {self.get_stat(wiredtiger.stat.conn.cache_scrub_restore)}')
        self.pr(f'cache_eviction_blocked_precise_checkpoint = {self.get_stat(wiredtiger.stat.conn.cache_eviction_blocked_precise_checkpoint)}')
        self.pr(f'checkpoint_pages_reconciled_bytes = {self.get_stat(wiredtiger.stat.conn.checkpoint_pages_reconciled_bytes)}')

        scrub_restore = self.get_stat(wiredtiger.stat.conn.cache_scrub_restore)
        self.assertGreater(scrub_restore, 0)
        self.assertGreater(
            self.get_stat(wiredtiger.stat.conn.checkpoint_pages_reconciled_bytes), self.nitems * 3 * 10)
        self.assertGreaterEqual(scrub_restore,
            self.get_stat(wiredtiger.stat.conn.cache_eviction_ahead_of_last_materialized_lsn))

        # Now let's also ensure that reconfigure does not preserve the last_materialized_lsn
        # across calls.
        self.conn.reconfigure(f'disaggregated=(last_materialized_lsn={last_lsn})')
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN,
                                               last_lsn + 10)
        self.conn.reconfigure(f'disaggregated=(role=leader)')

        # Ensure that the latest materialized LSN cannot go backwards.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN,
                                               last_lsn + 5))
