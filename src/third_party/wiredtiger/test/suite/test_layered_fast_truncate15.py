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

# test_layered_fast_truncate15.py
#   Validate edge scenario where no tombstones are written when ingest keys sit outside
#   the range. Follower truncate tombstones ingest keys only inside the range.

from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate15(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    """Follower truncate tombstones only ingest keys inside the range."""

    uris = [
        ("layered", {"uri": "layered:fast_truncate"}),
        ("table", {"uri": "table:fast_truncate"}),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)
    conn_config = 'disaggregated=(role="leader"),'

    def test_ingest_keys_flanking_range_not_tombstoned(self):
        # Ingest keys flank the range on both sides with none inside; neither should be tombstoned.
        self.setup_leader(keys=[0, 10, 20, 30])
        self.setup_follower(keys=[5, 25])
        self.truncate(10, 20)

        self.assertFalse(self.key_exists(10),
            "key 10 must be deleted (stable-only, inside truncate range)")
        self.assertTrue(self.key_exists(25),
            "key 25 must be visible (ingest key, outside truncate range)")

    def test_scan_correct_when_ingest_keys_flank_range(self):
        # Full scan with flanking ingest keys returns only keys outside the range.
        self.setup_leader(keys=[0, 10, 20, 30])
        self.setup_follower(keys=[5, 25])
        self.truncate(10, 20)

        self.assertEqual(self.visible_keys(), [0, 5, 25, 30])

    def test_ingest_key_only_below_range(self):
        # All ingest keys are below the range; none should be tombstoned.
        self.setup_leader(keys=[0, 5, 10, 15, 20])
        self.setup_follower(keys=[5])
        self.truncate(10, 15)

        self.assertFalse(self.key_exists(10), "key 10 must be deleted")
        self.assertTrue(self.key_exists(5), "key 5 must be visible")

    def test_ingest_key_only_above_range(self):
        # All ingest keys are above the range; none should be tombstoned.
        self.setup_leader(keys=[0, 5, 10, 15, 20])
        self.setup_follower(keys=[15])
        self.truncate(5, 10)

        self.assertFalse(self.key_exists(10), "key 10 must be deleted")
        self.assertTrue(self.key_exists(15), "key 15 must be visible")

    def test_multiple_ingest_keys_both_sides_no_ingest_in_range(self):
        # Multiple ingest keys on both sides of the range; none inside; all should stay visible.
        self.setup_leader(keys=[0, 5, 10, 15, 20, 25])
        self.setup_follower(keys=[3, 7, 18, 22])
        self.truncate(10, 15)

        for k in [10, 15]:
            self.assertFalse(self.key_exists(k),
                f"key {k} must be deleted (stable-only, inside truncate range)")
        for k in [3, 7, 18, 22]:
            self.assertTrue(self.key_exists(k),
                f"key {k} must be visible (ingest key, outside truncate range)")

if __name__ == "__main__":
    wttest.run()
