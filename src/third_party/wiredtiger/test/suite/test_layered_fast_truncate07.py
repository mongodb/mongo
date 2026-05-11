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

# test_layered_fast_truncate06.py
#   Follower-initiated truncate stores a bounded range in the truncate list.
#   Verifies NULL start/stop from the session API are resolved to the table's
#   first/last visible key, both via the verbose log line and by the row set
#   visible on subsequent reads.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_fast_truncate06(wttest.WiredTigerTestCase):

    conn_config = 'verbose=[layered:3],disaggregated=(role="leader"),'
    uri = 'layered:test_layered_fast_truncate06'

    key_formats = [
        ('string', dict(key_format='S')),
        ('int', dict(key_format='i')),
    ]
    disagg_storages = gen_disagg_storages('test_layered_fast_truncate06', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, key_formats)

    nitems = 100

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    # Zero-padded string keys so lexicographic order matches numeric order.
    def key(self, n):
        return f'{n:04d}' if self.key_format == 'S' else n

    # How the key is printed in the verbose log (via __wt_key_string).
    def key_str(self, n):
        return f'{n:04d}' if self.key_format == 'S' else str(n)

    def setup_follower(self):
        self.session.create(self.uri, f'key_format={self.key_format},value_format=S')
        self.insert_range(1, self.nitems)
        self.session.checkpoint()
        follower_config = ('verbose=[layered:3],disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")')
        self.reopen_conn(config=follower_config)

    def truncate(self, start=None, stop=None):
        c_start = c_stop = None
        if start is not None:
            c_start = self.session.open_cursor(self.uri)
            c_start.set_key(self.key(start))
        if stop is not None:
            c_stop = self.session.open_cursor(self.uri)
            c_stop.set_key(self.key(stop))

        # Use the table uri if both start and stop cursors are not given.
        uri = self.uri if (c_start is None and c_stop is None) else None
        self.session.begin_transaction()
        self.session.truncate(uri, c_start, c_stop, None)
        self.session.commit_transaction()
        if c_start is not None:
            c_start.close()
        if c_stop is not None:
            c_stop.close()

    def visible_keys(self, forward=True):
        c = self.session.open_cursor(self.uri)
        step = c.next if forward else c.prev
        keys = []
        while step() == 0:
            keys.append(c.get_key())
        c.close()
        return keys

    def insert_range(self, lo, hi):
        c = self.session.open_cursor(self.uri)
        for i in range(lo, hi + 1):
            self.session.begin_transaction()
            c[self.key(i)] = 'v'
            self.session.commit_transaction()
        c.close()

    # Keys in [1, nitems] minus [start, stop] (inclusive on both ends).
    def expected_keys(self, start, stop):
        return [self.key(i) for i in range(1, self.nitems + 1)
                if i < start or i > stop]

    # Assert the single truncate log line emitted with the concrete bounded range.
    def assert_trunc_log(self, start, stop):
        self.captureout.checkAdditionalPattern(self,
            f'inserting entry into truncate list on table {self.uri}: '
            f'start={self.key_str(start)} stop={self.key_str(stop)}')
        self.cleanStdout()

    def test_bounded_range(self):
        self.setup_follower()
        self.truncate(start=30, stop=60)
        self.assert_trunc_log(30, 60)
        self.assertEqual(self.visible_keys(), self.expected_keys(30, 60))

    def test_null_start_resolves_to_first_key(self):
        self.setup_follower()
        self.truncate(start=None, stop=60)
        self.assert_trunc_log(1, 60)
        self.assertEqual(self.visible_keys(), self.expected_keys(1, 60))

    def test_null_stop_resolves_to_last_key(self):
        self.setup_follower()
        self.truncate(start=30, stop=None)
        self.assert_trunc_log(30, self.nitems)
        self.assertEqual(self.visible_keys(), self.expected_keys(30, self.nitems))

    def test_both_null_is_full_table(self):
        self.setup_follower()
        self.truncate(start=None, stop=None)
        self.assert_trunc_log(1, self.nitems)
        self.assertEqual(self.visible_keys(), [])

    # An open-ended truncate captures "end" at commit time, not dynamically. Keys appended
    # after stop should be visible.
    def test_open_ended_truncate_does_not_hide_later_appends(self):
        self.setup_follower()
        self.truncate(start=80, stop=None)
        self.assert_trunc_log(80, self.nitems)
        self.insert_range(200, 210)
        expected = [self.key(i) for i in range(1, 80)] + \
                   [self.key(i) for i in range(200, 211)]
        self.assertEqual(self.visible_keys(), expected)

    def test_bounded_and_end_open_ended_overlap(self):
        self.setup_follower()
        self.truncate(start=20, stop=60)
        self.assert_trunc_log(20, 60)
        self.truncate(start=50, stop=None)
        # key 50-60 was deleted by the first truncate; search_near positions it on the
        # nearest in-bound key, 61.
        self.assert_trunc_log(61, self.nitems)
        expected = [self.key(i) for i in range(1, 20)]
        self.assertEqual(self.visible_keys(), expected)
        self.assertEqual(self.visible_keys(forward=False), list(reversed(expected)))

    def test_bounded_and_start_open_ended_overlap(self):
        self.setup_follower()
        self.truncate(start=20, stop=60)
        self.assert_trunc_log(20, 60)
        self.truncate(start=0, stop=30)
        # key 20-30 was deleted by the first truncate; search_near positions it on the
        # nearest live key, 19.
        self.assert_trunc_log(1, 19)
        expected = [self.key(i) for i in range(61, self.nitems + 1)]
        self.assertEqual(self.visible_keys(), expected)
        self.assertEqual(self.visible_keys(forward=False), list(reversed(expected)))
