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

import wiredtiger, wttest

# Range truncate statistics. Truncate a range that spans many on-disk leaf pages, some of
# which cannot be fast-deleted: pages with overflow values (not available in a disaggregated
# btree) or pages whose entries all carry a stop time. The pages that fall back to the slow
# path are counted, as are the parent internal pages dirtied by fast delete
# and their byte total. Pages already in memory are reported as a subset of the pages
# that fell back. The connection statistics must equal the sum of the per-table ones, a
# truncate on one table must not move another table's counters, and an ordinary update
# must not move them at all.
class test_truncate37(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=200MB,statistics=(fast)'
    uri = 'table:test_truncate37'
    other_uri = 'table:test_truncate37_other'
    config = 'key_format=i,value_format=S,allocation_size=512,leaf_page_max=4096'

    def get_stat(self, uri, name):
        cursor = self.session.open_cursor('statistics:' + uri)
        key = getattr(wiredtiger.stat.conn if not uri else wiredtiger.stat.dsrc, name)
        value = cursor[key][2]
        cursor.close()
        return value

    def truncate(self, uri, first, last, ts=None):
        start = self.session.open_cursor(uri)
        stop = self.session.open_cursor(uri)
        start.set_key(first)
        stop.set_key(last)
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)
        start.close()
        stop.close()
        if ts is None:
            self.session.commit_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

    # A disaggregated btree has no overflow items, so this case cannot make an on-disk page fall
    # back; test_range_truncate_stats_deleted covers that outcome there.
    @wttest.skip_for_hook("disagg", "a disaggregated btree has no overflow items")
    def test_range_truncate_stats(self):
        for uri in (self.uri, self.other_uri):
            self.session.create(uri, self.config)
            cursor = self.session.open_cursor(uri)
            for key in range(1, 10001):
                cursor[key] = 'a' * 100
            if uri == self.uri:
                cursor[5000] = 'a' * 12000
            cursor.close()
        self.session.checkpoint()
        self.reopen_conn()

        names = (
            'cursor_truncate_fast_delete_fallback_in_memory',
            'cursor_truncate_fast_delete_fallback_pages',
            'cursor_truncate_internal_bytes_dirtied',
            'cursor_truncate_internal_pages_dirtied',
        )
        before_conn = {name: self.get_stat('', name) for name in names}
        before_first = {name: self.get_stat(self.uri, name) for name in names}
        before_other = {name: self.get_stat(self.other_uri, name) for name in names}

        self.truncate(self.uri, 100, 9900)

        first = {name: self.get_stat(self.uri, name) - before_first[name] for name in names}
        for name in names:
            self.assertEqual(self.get_stat('', name) - before_conn[name], first[name])
            self.assertEqual(self.get_stat(self.other_uri, name), before_other[name])

        self.assertGreater(first['cursor_truncate_internal_pages_dirtied'], 0)
        self.assertGreater(first['cursor_truncate_internal_bytes_dirtied'], 0)
        self.assertGreater(
            first['cursor_truncate_fast_delete_fallback_pages'],
            first['cursor_truncate_fast_delete_fallback_in_memory'])

        cursor = self.session.open_cursor(self.other_uri)
        cursor[5000] = 'b' * 100
        cursor.close()
        for name in names:
            self.assertEqual(self.get_stat(self.other_uri, name), before_other[name])
        self.truncate(self.other_uri, 100, 9900)
        self.assertGreater(
            self.get_stat(self.other_uri, 'cursor_truncate_fast_delete_fallback_in_memory') -
            before_other['cursor_truncate_fast_delete_fallback_in_memory'], 0)
        for name in names:
            self.assertEqual(self.get_stat(self.uri, name) - before_first[name], first[name])
            self.assertEqual(
                self.get_stat('', name) - before_conn[name],
                first[name] + self.get_stat(self.other_uri, name) - before_other[name])

    # Pages whose entries all carry a stop time are rejected by range truncate even though they
    # have no overflow items, so they fall back to the slow path without being in memory.
    def test_range_truncate_stats_deleted(self):
        self.session.create(self.uri, self.config)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for key in range(1, 10001):
            cursor[key] = 'a' * 100
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.begin_transaction()
        for key in range(2001, 4001):
            cursor.set_key(key)
            cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.reopen_conn()

        in_memory = 'cursor_truncate_fast_delete_fallback_in_memory'
        pages = 'cursor_truncate_fast_delete_fallback_pages'
        skipped = 'cursor_truncate_fast_delete_pages_skipped_already_deleted'
        before_pages = self.get_stat(self.uri, pages)
        before_in_memory = self.get_stat(self.uri, in_memory)
        before_skipped = self.get_stat('', skipped)

        self.truncate(self.uri, 100, 9900, 20)

        self.assertGreater(self.get_stat('', skipped), before_skipped)
        self.assertGreater(
            self.get_stat(self.uri, pages) - before_pages,
            self.get_stat(self.uri, in_memory) - before_in_memory)
