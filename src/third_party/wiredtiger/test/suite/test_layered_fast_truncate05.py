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

import unittest
import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_fast_truncate05.py
#   Validate cursor::next_random behavior over fast-truncated ranges on a
#   standby (follower) node.
@disagg_test_class
class test_layered_fast_truncate05(wttest.WiredTigerTestCase):

    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate05')),
        ('table', dict(uri='table:test_layered_fast_truncate05')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_fast_truncate05', disagg_only=True)

    scenarios = make_scenarios(disagg_storages, uris)

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    # Total number of keys inserted. String keys are zero-padded to four
    # digits so that lexicographic order matches numeric order.
    nitems = 1000

    @staticmethod
    def key(n):
        return f'{n:04d}'

    def session_create_config(self):
        cfg = 'key_format=S,value_format=S'
        if self.uri.startswith('table'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    # Populate the table on the leader, checkpoint, then reopen as follower.
    def setup_follower(self):
        self.session.create(self.uri, self.session_create_config())
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[self.key(i)] = 'value'
            self.session.commit_transaction()
        cursor.close()
        self.session.checkpoint()

        follower_config = (
            'disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        )
        self.reopen_conn(config=follower_config)

    # Truncate the range [start, stop] (inclusive). If stop is None, truncate
    # from start to the end of the table.
    def truncate_range(self, start, stop):
        c1 = self.session.open_cursor(self.uri)
        c1.set_key(self.key(start))
        c2 = None
        if stop is not None:
            c2 = self.session.open_cursor(self.uri)
            c2.set_key(self.key(stop))
        self.session.begin_transaction()
        self.session.truncate(None, c1, c2, None)
        self.session.commit_transaction()
        c1.close()
        if c2 is not None:
            c2.close()

    # Draw `samples` random keys and assert none fall inside [low, high].
    def sample_assert_random(self, low, high, samples=200):
        cursor = self.session.open_cursor(self.uri, None, 'next_random=true')
        self.session.begin_transaction()
        for _ in range(samples):
            self.assertEqual(cursor.next(), 0, 'random cursor found no visible key')
            k = cursor.get_key()
            self.assertFalse(self.key(low) <= k <= self.key(high),
                f'random cursor returned truncated key {k}')
        self.session.rollback_transaction()
        cursor.close()

    def test_random_cursor_skips_truncated_range(self):
        # 200 random samples must all land outside the truncated range.
        self.setup_follower()
        self.truncate_range(100, 700)
        self.sample_assert_random(100, 700)

    # FIXME-WT-17133: random cursor inherits the same ingest-truncate gap.
    @unittest.skip("FIXME-WT-17133")
    def test_random_cursor_skips_truncated_range_with_live_ingest(self):
        # Update 200-400 on follower so those keys are live in ingest, then
        # truncate a range covering them. No random sample should leak.
        self.setup_follower()

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(200, 401):
            cursor[self.key(i)] = 'updated'
        self.session.commit_transaction()
        cursor.close()

        self.truncate_range(100, 700)
        self.sample_assert_random(100, 700)
