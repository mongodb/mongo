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

# test_layered_fast_truncate04.py
#   Validate cursor read-path behavior over fast-truncated ranges on a
#   standby (follower) node: next/prev scans, search_near positioning,
#   open-ended truncation, multiple truncated ranges, and mixed
#   update-then-truncate workloads.
@disagg_test_class
class test_layered_fast_truncate04(wttest.WiredTigerTestCase):

    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate04')),
        ('table', dict(uri='table:test_layered_fast_truncate04')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_fast_truncate04', disagg_only=True)

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

    # Return all keys visible via a forward scan.
    def scan_forward(self):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.session.rollback_transaction()
        cursor.close()
        return keys

    # Return all keys visible via a backward scan.
    def scan_backward(self):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        keys = []
        while cursor.prev() == 0:
            keys.append(cursor.get_key())
        self.session.rollback_transaction()
        cursor.close()
        return list(reversed(keys))  # reverse so order matches forward scan

    # Run search_near in its own transaction; return (exact, landed_key).
    def search_near(self, key):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor.set_key(self.key(key))
        exact = cursor.search_near()
        landed = cursor.get_key()
        self.session.rollback_transaction()
        cursor.close()
        return exact, landed

    # Run search in its own transaction; return the return value (0 or WT_NOTFOUND).
    def search(self, key):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor.set_key(self.key(key))
        ret = cursor.search()
        self.session.rollback_transaction()
        cursor.close()
        return ret

    # Assert forward and backward scans both return the expected key list.
    def assert_scan(self, expected):
        self.assertEqual(self.scan_forward(), expected, 'forward scan mismatch')
        self.assertEqual(self.scan_backward(), expected, 'backward scan mismatch')

    # Write a single key/value pair in its own transaction.
    def put(self, key, value='v'):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[self.key(key)] = value
        self.session.commit_transaction()
        cursor.close()

    def test_cursor_scan_skips_truncated_range(self):
        # Forward and backward scans must skip every key in the truncated range.
        self.setup_follower()
        self.truncate_range(100, 700)
        self.assert_scan([self.key(i) for i in range(self.nitems) if i < 100 or i > 700])

    def test_search_near_inside_truncated_range(self):
        # search_near for a key deep inside a truncated range must land outside
        # the range and must not report an exact match.
        self.setup_follower()
        self.truncate_range(100, 700)

        exact, landed = self.search_near(400)
        self.assertFalse(self.key(100) <= landed <= self.key(700),
            f'search_near landed inside truncated range at {landed}')
        self.assertNotEqual(exact, 0, 'exact=0 reported for a key in the truncated range')

    def test_search_near_with_live_ingest_inside_truncate(self):
        # The entire stable table is truncated, leaving only live ingest keys
        # as candidates for search_near. Test both directions by placing the
        # single visible ingest key above or below the search key.
        self.setup_follower()
        self.truncate_range(0, self.nitems - 1)

        # Scenario 1: ingest 0600 above search key 0500 forward (exact=1).
        self.put(600, 'ingest-live')
        self.assertEqual(self.search_near(500), (1, self.key(600)), 'forward scenario')

        # Scenario 2: remove 0600 and write 0400. Only live ingest key below
        # 0500 backward (exact=-1).
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor.set_key(self.key(600))
        cursor.remove()
        cursor[self.key(400)] = 'ingest-live'
        self.session.commit_transaction()
        cursor.close()

        self.assertEqual(self.search_near(500), (-1, self.key(400)), 'backward scenario')

    def test_search_near_at_truncate_boundary(self):
        # The start and stop keys of the range are inclusive, so search_near at
        # either boundary must land strictly outside the range.
        self.setup_follower()
        self.truncate_range(100, 700)

        for boundary in (100, 700):
            _, landed = self.search_near(boundary)
            self.assertFalse(self.key(100) <= landed <= self.key(700),
                f'boundary {self.key(boundary)} landed inside range at {landed}')

    def test_truncate_to_end_of_table(self):
        # Open-ended truncate from key 500; only 0-499 remain visible.
        self.setup_follower()
        self.truncate_range(500, None)
        self.assert_scan([self.key(i) for i in range(500)])

    def test_multiple_truncate_ranges(self):
        # Two disjoint bounded ranges; scans must skip both.
        self.setup_follower()
        self.truncate_range(100, 300)
        self.truncate_range(600, 800)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if not (100 <= i <= 300) and not (600 <= i <= 800)])

    def test_mixed_bounded_and_open_ended_truncates(self):
        # Bounded [100, 300] combined with open-ended [600, end]; 0-99 and 301-599 visible.
        self.setup_follower()
        self.truncate_range(100, 300)
        self.truncate_range(600, None)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if i < 100 or (301 <= i <= 599)])

    def test_open_ended_truncate_then_append_then_bounded_to_new_end(self):
        # Open-ended truncate captures a snapshot of "end" at commit time. Keys
        # appended afterwards are new data and must remain visible.
        self.setup_follower()
        self.truncate_range(800, None)

        for i in range(1000, 1100):
            self.put(i, 'appended')

        self.assert_scan([self.key(i) for i in range(800)]
                         + [self.key(i) for i in range(1000, 1100)])

    # FIXME-WT-17133: ingest truncate doesn't remove live ingest keys when the
    # start key is absent from ingest.
    @unittest.skip("FIXME-WT-17133")
    def test_mixed_truncate_and_update(self):
        # Update 200-400 on follower (lands as live committed values in ingest),
        # then truncate a range that covers them. Scans and search must hide them.
        self.setup_follower()
        for i in range(200, 401):
            self.put(i, 'updated')
        self.truncate_range(100, 700)

        self.assert_scan([self.key(i) for i in range(self.nitems) if i < 100 or i > 700])
        self.assertEqual(self.search(300), wiredtiger.WT_NOTFOUND,
            'search must hide an updated-then-truncated key')

    def test_search_returns_not_found_in_truncated_range(self):
        # search() goes through a different read path than scans and search_near;
        # both boundaries and interior keys must return WT_NOTFOUND.
        self.setup_follower()
        self.truncate_range(100, 700)

        for k in (400, 100, 700):
            self.assertEqual(self.search(k), wiredtiger.WT_NOTFOUND,
                f'search({self.key(k)}) inside range must be hidden')
        for k in (99, 701):
            self.assertEqual(self.search(k), 0,
                f'search({self.key(k)}) outside range must succeed')

    def test_search_near_direction_in_truncated_range(self):
        # search_near for a key inside a truncated range tries forward first and
        # falls back to backward if forward exhausts.
        self.setup_follower()

        # Bounded range [100, 700]. Forward finds 0701.
        self.truncate_range(100, 700)
        self.assertEqual(self.search_near(400), (1, self.key(701)), 'forward scenario')

        # Add open-ended truncate [800, end]. Forward exhausts, falls back to 0799.
        self.truncate_range(800, None)
        self.assertEqual(self.search_near(900), (-1, self.key(799)), 'backward scenario')

    def test_overlapping_truncated_ranges_scan(self):
        # Two overlapping ranges [100, 400] and [300, 700]: scans must skip the
        # full union [100, 700], not just one range at a time.
        self.setup_follower()
        self.truncate_range(100, 400)
        self.truncate_range(300, 700)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if i < 100 or i > 700])

    def test_entire_table_truncated(self):
        # Truncate every key; both scans must be empty.
        self.setup_follower()
        self.truncate_range(0, self.nitems - 1)
        self.assert_scan([])
