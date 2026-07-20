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

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios

# Verify that pending follower truncates land on stable when the follower steps up,
# across the variety of per-key shapes and edge cases.
@disagg_test_class
class test_layered_fast_truncate_stepup(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader")'
    uri = f'layered:{test_name}'
    nitems = 1000

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def populate_on_leader(self, ts=10):
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[i] = "v" + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

    def setup_follower(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.populate_on_leader()
        self.conn_follow, self.session_follow = self.open_follower()

    def write_kv(self, key, value, ts):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor[key] = value
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def remove_kv(self, key, ts):
        cursor = self.session_follow.open_cursor(self.uri, None, 'overwrite=false')
        cursor.set_key(key)
        self.session_follow.begin_transaction()
        cursor.remove()
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def truncate_range(self, start_key, stop_key, ts):
        c_start = self.session_follow.open_cursor(self.uri)
        c_start.set_key(start_key)
        c_stop = self.session_follow.open_cursor(self.uri)
        c_stop.set_key(stop_key)
        self.session_follow.begin_transaction()
        self.session_follow.truncate(None, c_start, c_stop, None)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        c_start.close()
        c_stop.close()

    def assert_visible(self, keys, value=None, ts=None):
        for k in keys:
            ret, val = self.search_at(self.session_follow, k, ts)
            self.assertEqual(ret, 0, f"key {k} should be visible at ts={ts}")
            if value is not None:
                expected = value(k) if callable(value) else value
                self.assertEqual(val, expected)

    def assert_deleted(self, keys, ts):
        for k in keys:
            ret, _ = self.search_at(self.session_follow, k, ts)
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND,
                f"key {k} should be deleted at ts={ts}")

    def assert_keys_gone(self, ranges):
        # Sweep the populated key space: keys inside any (lo, hi) inclusive range must be
        # deleted, keys outside must remain visible.
        cursor = self.session_follow.open_cursor(self.uri)
        for i in range(self.nitems):
            cursor.set_key(i)
            ret = cursor.search()
            in_range = any(lo <= i <= hi for lo, hi in ranges)
            if in_range:
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND, f"key {i} should be deleted")
            else:
                self.assertEqual(ret, 0, f"key {i} should remain visible")
        cursor.close()

    # Single truncate, stable-only keys: the simplest case.
    def test_stable_only_keys(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_keys_gone([(100, 700)])

    # Truncated range mixes follower-updated keys and stable-only keys.
    def test_mixed_keys(self):
        self.setup_follower()
        for i in [200, 300, 400, 500, 600]:
            self.write_kv(i, "follower-update", 15)
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_keys_gone([(100, 700)])

    # Reinsert after the truncate must survive step-up.
    def test_truncate_then_reinsert(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.write_kv(300, "reinserted", 25)
        self.step_up()
        self.assert_visible([300], "reinserted", ts=100)
        self.assert_deleted([100, 150, 250, 400, 500, 700], ts=100)

    # --- Range boundaries ---

    def test_single_key_truncate(self):
        self.setup_follower()
        self.truncate_range(500, 500, 20)
        self.step_up()
        self.assert_keys_gone([(500, 500)])

    def test_truncate_at_table_start(self):
        self.setup_follower()
        self.truncate_range(0, 50, 20)
        self.step_up()
        self.assert_keys_gone([(0, 50)])

    def test_truncate_at_table_end(self):
        self.setup_follower()
        self.truncate_range(950, 999, 20)
        self.step_up()
        self.assert_keys_gone([(950, 999)])

    def test_truncate_full_table(self):
        self.setup_follower()
        self.truncate_range(0, 999, 20)
        self.step_up()
        self.assert_keys_gone([(0, 999)])

    # Range fully outside the populated key space.
    def test_truncate_empty_range(self):
        self.setup_follower()
        self.truncate_range(2000, 3000, 20)
        self.step_up()
        self.assert_visible([0, 100, 500, 999], lambda k: f"v{k}", ts=100)

    # --- Multiple truncates ---

    # Multiple non-overlapping truncates.
    def test_multiple_truncates(self):
        self.setup_follower()
        self.truncate_range(100, 200, 20)
        self.truncate_range(400, 500, 25)
        self.truncate_range(800, 900, 30)
        self.step_up()
        self.assert_keys_gone([(100, 200), (400, 500), (800, 900)])

    # Same range truncated twice.
    def test_duplicate_truncates(self):
        self.setup_follower()
        self.truncate_range(200, 500, 20)
        self.truncate_range(200, 500, 25)
        self.step_up()
        self.assert_keys_gone([(200, 500)])

    # Truncate, reinsert, then re-truncate. Each layer's effect must hold at its own ts.
    def test_truncate_reinsert_truncate(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.write_kv(300, "reinserted", 25)
        self.truncate_range(100, 700, 30)
        self.step_up()
        self.assert_deleted([100, 300, 500, 700], ts=100)
        self.assert_deleted([300], ts=22)
        self.assert_visible([300], "reinserted", ts=27)

    # --- Snapshot reads at intermediate timestamps ---

    # Reads before vs after a single truncate commit ts.
    def test_snapshot_read_around_truncate(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_visible([100, 250, 500, 700], lambda k: f"v{k}", ts=15)
        self.assert_deleted([100, 250, 500, 700], ts=30)
        self.assert_visible([50, 800], ts=30)

    # Read at exactly the truncate commit timestamp.
    def test_read_at_truncate_timestamp(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_deleted([100, 250, 500, 700], ts=20)

    # Stable-only key truncated then reinserted: gap read must show deleted.
    def test_intermediate_read_truncate_then_reinsert(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.write_kv(300, "reinserted", 25)
        self.step_up()
        self.assert_deleted([300], ts=22)

    # Overlapping truncates at distinct timestamps: gap read sees only the first.
    def test_intermediate_read_overlapping_truncates(self):
        self.setup_follower()
        self.truncate_range(100, 400, 20)
        self.truncate_range(300, 600, 30)
        self.step_up()
        self.assert_deleted([100, 200, 350, 400], ts=25)
        self.assert_visible([450, 500, 600], ts=25)
        self.assert_deleted([100, 250, 350, 450, 600], ts=35)

    # Mixed per-key history (stable-only, follower-updated, with and without reinsert) in
    # one truncate range.
    def test_intermediate_read_mixed_per_key_history(self):
        self.setup_follower()
        for i in [200, 400]:
            self.write_kv(i, "follower-pre", 15)
        self.truncate_range(100, 700, 20)
        self.write_kv(300, "reinserted-stable-only", 25)
        self.write_kv(400, "reinserted-follower-updated", 25)
        self.step_up()
        self.assert_deleted([100, 200, 300, 400, 500, 700], ts=22)
        self.assert_visible([300], "reinserted-stable-only", ts=30)
        self.assert_visible([400], "reinserted-follower-updated", ts=30)
        self.assert_deleted([100, 200, 500, 700], ts=30)

    # --- Step-up edge cases ---

    # No follower truncates: replay must be a no-op.
    def test_stepup_with_empty_truncate_list(self):
        self.setup_follower()
        self.step_up()
        self.assert_visible([0, 100, 500, 999], lambda k: f"v{k}", ts=100)

    # New leader writes to the freshly-truncated range.
    def test_post_stepup_writes_to_truncated_range(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.write_kv(300, "after-stepup", 30)
        self.assert_visible([300], "after-stepup", ts=100)

    # Ingest key sits at the exact start bound of the truncate range. The sliding window
    # algorithm must not include the start key in a stable window; the ingest drain owns it.
    def test_ingest_key_at_start_bound(self):
        self.setup_follower()
        self.write_kv(100, "ingest-at-start", 15)
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_keys_gone([(100, 700)])

    # Ingest key sits at the exact stop bound of the truncate range.
    def test_ingest_key_at_stop_bound(self):
        self.setup_follower()
        self.write_kv(700, "ingest-at-stop", 15)
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_keys_gone([(100, 700)])

    # Ingest keys at both start and stop bounds, with stable-only keys in between.
    def test_ingest_keys_at_both_bounds(self):
        self.setup_follower()
        self.write_kv(100, "ingest-at-start", 15)
        self.write_kv(700, "ingest-at-stop", 15)
        self.truncate_range(100, 700, 20)
        self.step_up()
        self.assert_keys_gone([(100, 700)])

    # Step-up bypasses commit timestamp ordering checks when draining a past truncate.
    def test_drain_truncate_below_stable_timestamp(self):
        self.setup_follower()
        self.truncate_range(100, 700, 20)
        # Advance the follower's stable timestamp past the truncate commit ts.
        self.conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.step_up()
        # The drain succeeded; truncated keys are deleted at any post-stable read.
        self.assert_deleted([100, 250, 500, 700], ts=35)
        self.assert_visible([50, 800], lambda k: f"v{k}", ts=35)

    # --- Layered remove + truncate (special ingest tombstones) ---

    # Follower remove with no follower truncate.
    def test_layered_remove_only(self):
        self.setup_follower()
        self.remove_kv(300, 20)
        self.step_up()
        self.assert_deleted([300], ts=100)
        self.assert_visible([300], "v300", ts=15)

    # Staggered remove then truncate.
    def test_layered_remove_then_truncate(self):
        self.setup_follower()
        self.remove_kv(300, 20)
        self.truncate_range(100, 700, 25)
        self.step_up()
        for ts in [20, 22, 25, 30]:
            self.assert_deleted([300], ts=ts)
        self.assert_visible([300], "v300", ts=15)
        self.assert_deleted([100, 250, 500, 700], ts=100)

    # Remove and truncate at the same commit ts: replay's snapshot must include start_ts
    # to see the remove's ingest tombstone and not stack a redundant stable tombstone.
    def test_layered_remove_and_truncate_same_ts(self):
        self.setup_follower()
        self.remove_kv(300, 20)
        self.truncate_range(100, 700, 20)
        self.step_up()
        for ts in [20, 22, 30]:
            self.assert_deleted([300], ts=ts)
        self.assert_visible([300], "v300", ts=19)
        self.assert_deleted([100, 250, 500, 700], ts=100)

    # Truncate covers a key whose only ingest entry is a tombstone from an earlier remove.
    def test_truncate_over_pre_existing_remove(self):
        self.setup_follower()
        self.remove_kv(300, 15)
        self.truncate_range(100, 700, 30)
        self.step_up()
        for ts in [15, 20, 30, 40]:
            self.assert_deleted([300], ts=ts)
        self.assert_visible([300], "v300", ts=12)
