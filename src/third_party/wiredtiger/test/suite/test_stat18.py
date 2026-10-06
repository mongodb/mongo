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

# Globally obsolete inline value bytes reported by the statistics=(all) tree walk.
class test_stat18(wttest.WiredTigerTestCase):
    uri = 'table:test_stat18'
    conn_config = 'statistics=(all)'
    live = b'L' * 64
    obsolete = b'O' * (50 * 1024)

    def _set_ts(self, oldest, stable):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(oldest) +
            ',stable_timestamp=' + self.timestamp_str(stable))

    # Write values at 10, remove some at 20, and persist while the removes are not yet
    # globally visible so the removed values stay in the leaf image with a stop time.
    def _populate(self, values, remove, config=''):
        self.session.create(self.uri,
            'key_format=Q,value_format=u,leaf_page_max=32KB,leaf_value_max=64MB,' + config)
        self._set_ts(1, 1)
        c = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=10):
            for k, v in values.items():
                c[k] = v
        self._set_ts(10, 10)
        self.session.checkpoint()
        with self.transaction(commit_timestamp=20):
            for k in remove:
                c.set_key(k)
                self.assertEqual(c.remove(), 0)
        c.close()
        self._set_ts(10, 20)
        self.session.checkpoint()
        self.reopen_conn()

    def _populate_mixed(self):
        self._populate({1: self.live, 2: self.obsolete}, [2])

    def test_mixed_page(self):
        self._populate_mixed()
        self._set_ts(30, 30)
        c = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_analyzed][2], 1)
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_pages][2], 1)
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes][2], len(self.obsolete))
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes_mixed][2], len(self.obsolete))
        c.close()

    def test_fully_obsolete_page(self):
        self._populate({1: self.obsolete, 2: self.obsolete}, [1, 2])
        self._set_ts(30, 30)
        c = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes][2], 2 * len(self.obsolete))
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes_mixed][2], 0)
        c.close()

    # Dictionary compression shares one value between a removed key and a live key. The page stays
    # mixed once the remove is globally visible.
    def test_live_dictionary_copy_marks_page_mixed(self):
        shared = b'S' * 128
        removed = b'R' * 128
        self.session.create(self.uri,
            'key_format=Q,value_format=u,leaf_page_max=32KB,dictionary=100')
        self._set_ts(1, 1)
        c = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=10):
            c[1] = shared
            c[2] = shared
            c[3] = removed
        self._set_ts(10, 10)
        self.session.checkpoint()
        with self.transaction(commit_timestamp=20):
            for k in (1, 3):
                c.set_key(k)
                self.assertEqual(c.remove(), 0)
        c.close()
        self._set_ts(10, 20)
        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        dictionary_before = stat_cursor[stat.dsrc.rec_dictionary][2]
        stat_cursor.close()
        self.session.checkpoint()
        stat_cursor = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        self.assertGreater(stat_cursor[stat.dsrc.rec_dictionary][2], dictionary_before)
        stat_cursor.close()
        self.reopen_conn()
        self._set_ts(30, 30)
        c = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')
        obsolete = len(shared) + len(removed)
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_pages][2], 1)
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes][2], obsolete)
        self.assertEqual(c[stat.dsrc.btree_obsolete_inline_bytes_mixed][2], obsolete)
        c.close()
