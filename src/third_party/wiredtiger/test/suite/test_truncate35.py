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

# A table's write_timestamp_usage policy can be relaxed to "never" with WT_SESSION::alter
# after the table already has real, timestamped history: alter only rewrites metadata, it
# never inspects or rewrites existing content. Once that happens, a table-wide (or range)
# fast truncate committed with no timestamp is no longer validated against the timestamped
# history underneath it, and can discard a page without ever clearing the history store
# records that page's superseded values live in.
@wttest.skip_for_hook("disagg", "disaggregated storage rejects write_timestamp_usage=never")
@wttest.skip_for_hook("tiered", "tiered truncate does not take the fast-delete path")
class test_truncate35(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=200MB,statistics=(all)'
    uri = 'table:test_truncate35'
    create_cfg = 'key_format=i,value_format=S,leaf_page_max=4KB'

    value = 'abcdefghijklmnopqrstuvwxyz' * 3
    nrows = 2000

    def get_stat(self, statname):
        c = self.session.open_cursor('statistics:')
        val = c[statname][2]
        c.close()
        return val

    def hs_has_window(self, start_ts, stop_ts):
        c = self.session.open_cursor('file:WiredTigerHS.wt')
        found = False
        for _bid, _key, hs_start_ts, _cnt, hs_stop_ts, *_rest in c:
            if hs_start_ts == start_ts and hs_stop_ts == stop_ts:
                found = True
                break
        c.close()
        return found

    def test_truncate35(self):
        self.session.create(self.uri, self.create_cfg)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cursor[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cursor[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # Advance stable, but not oldest: the first value is genuinely superseded, not obsolete.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        # Push both values to disk without closing the connection: closing marks the
        # connection as shutting down, at which point everything is treated as globally
        # visible and the history store gets cleared regardless of the bug under test.
        ev = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        for i in range(1, self.nrows + 1):
            ev.set_key(i)
            ev.search()
            ev.reset()
        self.session.rollback_transaction()
        ev.close()

        self.assertTrue(self.hs_has_window(10, 20),
            'setup did not push the superseded value to the history store')

        # A pure metadata rewrite: it does not touch the leaf or the history store.
        self.session.alter(self.uri, 'write_timestamp_usage=never')

        fast_before = self.get_stat(stat.conn.rec_page_delete_fast)
        hs_clear_before = self.get_stat(stat.conn.cache_hs_key_truncate)

        # Ordinary usage for a table now configured "never": no timestamp at all, and no
        # need for no_timestamp=true -- that flag is an escape hatch for ordered tables.
        self.session.begin_transaction()
        start = self.session.open_cursor(self.uri)
        start.set_key(5)
        self.session.truncate(None, start, None, None)
        start.close()
        self.session.commit_transaction()

        self.assertGreater(self.get_stat(stat.conn.rec_page_delete_fast), fast_before,
            'truncate did not take the fast path')
        self.assertEqual(self.get_stat(stat.conn.cache_hs_key_truncate), hs_clear_before,
            'a per-key history store clear ran; the fast path was not actually exercised')

        # Force reconciliation of the parent so the now globally-visible page_del
        # information gets discarded.
        self.session.checkpoint()
        self.session.checkpoint()

        # The stale, real-timestamped history store entry should have been cleared by
        # the truncate. It was not: the fast path never visited it.
        self.assertTrue(self.hs_has_window(10, 20),
            'history store entry was cleared -- the gap did not reproduce')
