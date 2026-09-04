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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# Checkpoint cleanup dirties a page with obsolete time window information so reconciliation can
# strip it out. On a disaggregated btree, that dirty adds no new update, so reconciliation's
# skip-write optimization finds nothing newer than what's already durable and writes nothing at
# all -- the obsolete time window stays on disk regardless. Verify checkpoint cleanup no longer
# reads or dirties pages for that reason on disaggregated btrees, while page-level cleanup
# (fast-deleting a fully obsolete page) keeps working.
@disagg_test_class
class test_layered_checkpoint_cleanup02(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = 'table:test_layered_checkpoint_cleanup02'
    # Small pages so a key range spans several whole pages, and so the tree has internal pages
    # for checkpoint cleanup to walk through.
    create_params = ('key_format=i,value_format=S,block_manager=disagg,log=(enabled=false),'
      'allocation_size=512,leaf_page_max=512,internal_page_max=512,memory_page_max=4096')
    nrows = 1000
    value = 'a' * 50

    def force_checkpoint_cleanup(self):
        prev_success = self.get_stat(stat.conn.checkpoint_cleanup_success)
        self.session.checkpoint('debug=(checkpoint_cleanup=true)')
        self.assertStatGreaterSoon(stat.conn.checkpoint_cleanup_success, prev_success, timeout=30)

    def evict_all(self):
        # Evict every page to disk, a requirement for both fast-delete and checkpoint cleanup's
        # obsolete time window checks on on-disk pages.
        with wttest.open_cursor(
          self.session, self.uri, config='debug=(release_evict)') as evict_cursor, \
          self.transaction(rollback=True):
            for k in range(1, self.nrows + 1):
                evict_cursor.set_key(k)
                evict_cursor.search()
                evict_cursor.reset()

    def test_obsolete_time_window_not_dirtied(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.create_params)
        with wttest.open_cursor(self.session, self.uri) as cursor, \
          self.transaction(commit_timestamp=10):
            for k in range(1, self.nrows + 1):
                cursor[k] = self.value
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.evict_all()

        # Move the oldest timestamp forward so every start time point becomes globally visible:
        # exactly the condition key-level cleanup would otherwise dirty pages for.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        self.force_checkpoint_cleanup()

        self.assertGreater(self.get_stat(stat.dsrc.checkpoint_cleanup_pages_visited, self.uri), 0)
        self.assertEqual(
          self.get_stat(stat.dsrc.checkpoint_cleanup_pages_read_obsolete_tw, self.uri), 0)
        self.assertEqual(self.get_stat(stat.dsrc.checkpoint_cleanup_pages_obsolete_tw, self.uri), 0)

    def test_fast_deleted_page_still_removed(self):
        trunc_start, trunc_stop = 100, 300

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.create_params)
        with wttest.open_cursor(self.session, self.uri) as cursor, \
          self.transaction(commit_timestamp=10):
            for k in range(1, self.nrows + 1):
                cursor[k] = self.value
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.evict_all()

        with wttest.open_cursor(self.session, self.uri) as start_cursor, \
          wttest.open_cursor(self.session, self.uri) as stop_cursor, \
          self.transaction(commit_timestamp=20):
            start_cursor.set_key(trunc_start)
            stop_cursor.set_key(trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)
        fast_deleted = self.get_stat(stat.dsrc.rec_page_delete_fast, self.uri)
        self.assertGreater(fast_deleted, 0, "truncate did not fast-delete any pages")

        removed_before = self.get_stat(stat.dsrc.checkpoint_cleanup_pages_removed, self.uri)

        # Make the truncation globally visible and run checkpoint cleanup: page-level cleanup
        # is unaffected by the disagg guard on key-level cleanup, so the fast-deleted pages are
        # still reclaimed.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20) +
          ',oldest_timestamp=' + self.timestamp_str(20))
        self.force_checkpoint_cleanup()

        self.assertGreaterEqual(
          self.get_stat(stat.dsrc.checkpoint_cleanup_pages_removed, self.uri) - removed_before,
          fast_deleted)
