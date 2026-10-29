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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# A dirty disaggregated leaf that reconciles to a skip-write single-block REPLACE while
# ahead of the materialization frontier must be kept in cache, not discarded. Reproduces
# the case where an obsolete-time-window cleanup dirties such a page, the skip-write REPLACE
# path drops the scrubbed disk image, eviction discards the page, and a subsequent read
# faults it back in ahead of the frontier.
@disagg_test_class
class test_layered_eviction06(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = 'statistics=(all),disaggregated=(role="leader")'

    create_session_config = 'key_format=i,value_format=S'

    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Force a read-and-evict of the page holding the given key. The session-level
    # release_evict_page debug flag is required to let eviction run the obsolete-time-window
    # review (application threads otherwise skip it).
    def evict(self, key):
        evict_session = self.conn.open_session('debug=(release_evict_page=true)')
        evict_session.begin_transaction('ignore_prepare=true')
        evict_cursor = evict_session.open_cursor(self.uri, None, None)
        evict_cursor.set_key(key)
        self.assertEqual(evict_cursor.search(), 0)
        evict_cursor.reset()
        evict_cursor.close()
        evict_session.rollback_transaction()
        evict_session.close()

    def advance_frontier(self, page_log):
        (ret, last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)
        page_log.pl_set_last_materialized_lsn(self.session, last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, last_lsn)
        return last_lsn

    def test_layered_eviction06(self):
        page_log = self.conn.get_page_log(self.vars.page_log)
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri)

        # Insert a key with a commit timestamp. There is no delete, so the leaf has no stop time
        # window - only a start time window that becomes obsolete once oldest advances past it.
        self.session.begin_transaction()
        cursor[1] = 'first'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # First checkpoint writes the page to disaggregated storage. Advance the materialization
        # frontier to this checkpoint so the page is materialized.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        lsn1 = self.advance_frontier(page_log)
        self.pr(f'lsn1 (frontier) = {lsn1}')

        # Insert a second key on the same leaf, dirtying it. Do NOT checkpoint and do NOT advance
        # the frontier.
        self.session.begin_transaction()
        cursor[2] = 'second'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # First eviction: the dirty disaggregated leaf is written ahead of the frontier and, under
        # forced scrub, retained in cache via a split_rewrite. This is the legal "evict ahead of
        # the frontier" case. The re-instantiated page is fresh, so its reconciliation result is
        # reset (rec_result == 0) while its backing image stays ahead of the frontier.
        self.evict(1)

        # Advance oldest past both start timestamps so the leaf's start time window is obsolete.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30))

        # Second eviction: the obsolete-time-window review dirties the fresh page; reconciliation
        # under forced scrub finds no key-changing updates and takes the skip-write single-block
        # REPLACE path. Because the previous reconciliation result was reset, no prior block cookie
        # is copied (the "cookie:0" case), so without the fix no disk image is retained and the
        # page is discarded from cache while its backing image is still ahead of the frontier.
        self.evict(1)

        read_ahead_before = self.get_stat(stat.conn.disagg_block_read_ahead_frontier)

        # Read the key back from the stable (disaggregated) constituent directly. A read through
        # the layered URI is served by the in-memory ingest table and never touches the stable
        # leaf; reading the stable file forces the disaggregated block read. If the page was
        # wrongly discarded by the skip-write eviction, this faults it in from storage at an LSN
        # ahead of the materialization frontier.
        stable_uri = 'file:' + self.uri.split(':', 1)[1] + '.wt_stable'
        read_session = self.conn.open_session()
        read_cursor = read_session.open_cursor(stable_uri)
        read_cursor.set_key(1)
        self.assertEqual(read_cursor.search(), 0)
        self.assertEqual(read_cursor.get_value(), 'first')
        read_cursor.close()
        read_session.close()

        # The page should have stayed in cache: no read ahead of the frontier should occur.
        self.assertEqual(self.get_stat(stat.conn.disagg_block_read_ahead_frontier), read_ahead_before)

        # Checkpoint and advance the frontier past the current LSN so tearDown
        # verifyLayered can read all pages without tripping the frontier check,
        # which would otherwise prevent the disaggregated layer from completing
        # its reads and releasing the dhandle for exclusive verify access.
        self.session.checkpoint()
        (ret, current_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)
        page_log.pl_set_last_materialized_lsn(self.session, current_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, current_lsn)
