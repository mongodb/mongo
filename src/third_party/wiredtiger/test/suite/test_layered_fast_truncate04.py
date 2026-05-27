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
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios

# test_layered_fast_truncate04.py
#   Validate cursor read-path behavior over fast-truncated ranges on a
#   standby (follower) node: next/prev scans, search_near positioning,
#   open-ended truncation, multiple truncated ranges, and mixed
#   update-then-truncate workloads.
@disagg_test_class
class test_layered_fast_truncate04(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):

    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate04')),
        ('table', dict(uri='table:test_layered_fast_truncate04')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_fast_truncate04', disagg_only=True)

    scenarios = make_scenarios(disagg_storages, uris)

    # Total number of keys inserted. String keys are zero-padded to four
    # digits so that lexicographic order matches numeric order.
    nitems = 1000

    def key(self, n):
        return f'{n:04d}'

    def session_create_config(self):
        cfg = 'key_format=S,value_format=S'
        if self.uri.startswith('table'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    # Populate the table on the leader, checkpoint, then reopen as follower.
    def setup_follower(self):
        self.setup_leader(keys=range(self.nitems))
        super().setup_follower()

    # Return all keys visible via a forward and a backward scan; assert both
    # match the expected list.
    def assert_scan(self, expected):
        self.assertEqual(self.visible_keys(), expected, 'forward scan mismatch')
        self.assertEqual(list(reversed(self.visible_keys(forward=False))), expected,
            'backward scan mismatch')

    # Run search_near in its own transaction; return (exact, landed_key).
    def search_near(self, key):
        return self.search_near_key(key)

    # Write a single key/value pair in its own transaction.
    def put(self, key, value='v'):
        self.populate([key], value=value)

    def test_cursor_scan_skips_truncated_range(self):
        # Forward and backward scans must skip every key in the truncated range.
        self.setup_follower()
        self.truncate(100, 700)
        self.assert_scan([self.key(i) for i in range(self.nitems) if i < 100 or i > 700])

    def test_search_near_inside_truncated_range(self):
        # search_near for a key deep inside a truncated range must land outside
        # the range and must not report an exact match.
        self.setup_follower()
        self.truncate(100, 700)

        exact, landed = self.search_near(400)
        self.assertFalse(self.key(100) <= landed <= self.key(700),
            f'search_near landed inside truncated range at {landed}')
        self.assertNotEqual(exact, 0, 'exact=0 reported for a key in the truncated range')

    def test_search_near_with_live_ingest_inside_truncate(self):
        # The entire stable table is truncated, leaving only live ingest keys
        # as candidates for search_near. Test both directions by placing the
        # single visible ingest key above or below the search key.
        self.setup_follower()
        self.truncate(0, self.nitems - 1)

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
        self.truncate(100, 700)

        for boundary in (100, 700):
            _, landed = self.search_near(boundary)
            self.assertFalse(self.key(100) <= landed <= self.key(700),
                f'boundary {self.key(boundary)} landed inside range at {landed}')

    def test_truncate_to_end_of_table(self):
        # Open-ended truncate from key 500; only 0-499 remain visible.
        self.setup_follower()
        self.truncate(500, None)
        self.assert_scan([self.key(i) for i in range(500)])

    def test_multiple_truncate_ranges(self):
        # Two disjoint bounded ranges; scans must skip both.
        self.setup_follower()
        self.truncate(100, 300)
        self.truncate(600, 800)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if not (100 <= i <= 300) and not (600 <= i <= 800)])

    def test_mixed_bounded_and_open_ended_truncates(self):
        # Bounded [100, 300] combined with open-ended [600, end]; 0-99 and 301-599 visible.
        self.setup_follower()
        self.truncate(100, 300)
        self.truncate(600, None)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if i < 100 or (301 <= i <= 599)])

    def test_open_ended_truncate_then_append_then_bounded_to_new_end(self):
        # Open-ended truncate captures a snapshot of "end" at commit time. Keys
        # appended afterwards are new data and must remain visible.
        self.setup_follower()
        self.truncate(800, None)

        for i in range(1000, 1100):
            self.put(i, 'appended')

        self.assert_scan([self.key(i) for i in range(800)]
                         + [self.key(i) for i in range(1000, 1100)])

    def test_mixed_truncate_and_update(self):
        # Update 200-400 on follower (lands as live committed values in ingest),
        # then truncate a range that covers them. Scans and search must hide them.
        self.setup_follower()
        for i in range(200, 401):
            self.put(i, 'updated')
        self.truncate(100, 700)

        self.assert_scan([self.key(i) for i in range(self.nitems) if i < 100 or i > 700])
        self.assertFalse(self.key_exists(300),
            'search must hide an updated-then-truncated key')

    def test_search_returns_not_found_in_truncated_range(self):
        # search() goes through a different read path than scans and search_near;
        # both boundaries and interior keys must return WT_NOTFOUND.
        self.setup_follower()
        self.truncate(100, 700)

        for k in (400, 100, 700):
            self.assertFalse(self.key_exists(k),
                f'search({self.key(k)}) inside range must be hidden')
        for k in (99, 701):
            self.assertTrue(self.key_exists(k),
                f'search({self.key(k)}) outside range must succeed')

    def test_search_near_direction_in_truncated_range(self):
        # search_near for a key inside a truncated range tries forward first and
        # falls back to backward if forward exhausts.
        self.setup_follower()

        # Bounded range [100, 700]. Forward finds 0701.
        self.truncate(100, 700)
        self.assertEqual(self.search_near(400), (1, self.key(701)), 'forward scenario')

        # Add open-ended truncate [800, end]. Forward exhausts, falls back to 0799.
        self.truncate(800, None)
        self.assertEqual(self.search_near(900), (-1, self.key(799)), 'backward scenario')

    def test_overlapping_truncated_ranges_scan(self):
        # Two overlapping ranges [100, 400] and [300, 700]: scans must skip the
        # full union [100, 700], not just one range at a time.
        self.setup_follower()
        self.truncate(100, 400)
        self.truncate(300, 700)
        self.assert_scan([self.key(i) for i in range(self.nitems)
                          if i < 100 or i > 700])

    def test_entire_table_truncated(self):
        # Truncate every key; both scans must be empty.
        self.setup_follower()
        self.truncate(0, self.nitems - 1)
        self.assert_scan([])
