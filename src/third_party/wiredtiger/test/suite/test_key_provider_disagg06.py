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

import wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_key_provider import KeyProviderBase
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Push-mode key provider scenarios:
# - persist the newest key at or below stable
# - future keys wait for stable to catch up
# - reject empty, non-increasing, or stale keys
# - persisted key survives restart
@disagg_test_class
class test_key_provider_disagg06(KeyProviderBase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f"layered:{test_name}"

    def populate_table(self):
        # A populated table gives every checkpoint real work to flush.
        self.dataset = SimpleDataSet(self, self.uri, 10)
        self.dataset.populate()
        self.row = 10

    def write_and_checkpoint(self):
        # A checkpoint only runs the key provider when there is dirty data; reopen the cursor per
        # call to survive restarts.
        self.row += 1
        cursor = self.session.open_cursor(self.uri)
        cursor[self.dataset.key(self.row)] = self.dataset.value(self.row)
        cursor.close()
        self.session.checkpoint()

    def test_select_highest_at_or_below_checkpoint(self):
        self.populate_table()

        # Pick the highest queued timestamp at or below stable; future keys are retained.
        self.push_crypt_key(1)
        self.push_crypt_key(2)
        self.push_crypt_key(5)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        self.write_and_checkpoint()
        self.validate_latest_kek(2)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(5)

    def test_drain_as_stable_advances(self):
        self.populate_table()

        # Each checkpoint consumes a prefix of the queue and advances, writing a new page each time.
        for ts in range(1, 6):
            self.push_crypt_key(ts)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.write_and_checkpoint()
        self.validate_latest_kek(2)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(4))
        self.write_and_checkpoint()
        self.validate_latest_kek(4)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(5)

        # Three new pages with strictly increasing LSN.
        rows = self.fetch_key_provider_pages()
        self.assertEqual(len(rows), 3)
        lsns = [r['lsn'] for r in rows]
        self.assertEqual(lsns, sorted(lsns))
        self.assertEqual(len(set(lsns)), 3)

    def test_future_keys_wait_for_stable(self):
        self.populate_table()

        # A checkpoint whose only queued key is in the future writes nothing new.
        self.push_crypt_key(1)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.write_and_checkpoint()
        self.validate_latest_kek(1)
        self.assertEqual(self.key_provider_page_count(), 1)

        self.push_crypt_key(10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(1)
        self.assertEqual(self.key_provider_page_count(), 1)

    def test_at_or_below_stable(self):
        self.populate_table()

        # Reject a timestamp at or below the global stable timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(10), '/set_key.*Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(5), '/set_key.*Invalid argument/')
        self.push_crypt_key(11)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(11))
        self.write_and_checkpoint()
        self.validate_latest_kek(11)

    def test_reject_decreasing_timestamp(self):
        self.populate_table()

        # Reject a timestamp not strictly above the last queued key.
        self.push_crypt_key(5)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(5), '/set_key.*Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(4), '/set_key.*Invalid argument/')
        self.push_crypt_key(6)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(6))
        self.write_and_checkpoint()
        self.validate_latest_kek(6)

    def test_empty_key_buffer(self):
        self.populate_table()

        # Reject an empty key buffer, leaving the queue unchanged.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(5, key=b''), '/set_key.*Invalid argument/')
        self.push_crypt_key(5)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(5)

    def test_monotonic_resets_after_drain(self):
        self.populate_table()

        # The decreasing rule is relative to the live queue, which is empty after a drain.
        self.push_crypt_key(5)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(5)

        # A fresh push at 6 only has to beat stable; the drained tail does not count.
        self.push_crypt_key(6)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(6))
        self.write_and_checkpoint()
        self.validate_latest_kek(6)

    def test_zero_timestamp(self):
        self.populate_table()

        # A zero timestamp is not strictly above the zero stable timestamp.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.push_crypt_key(0), '/set_key.*Invalid argument/')

        # A subsequent valid push is unaffected and persists.
        self.push_crypt_key(1)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.write_and_checkpoint()
        self.validate_latest_kek(1)

    def test_load_key_round_trips_timestamp_across_restart(self):
        self.populate_table()

        # The persisted key survives repeated restarts; a greater push is selected after each reopen.
        for ts in (7, 8, 9):
            self.push_crypt_key(ts)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
            self.write_and_checkpoint()
            self.validate_latest_kek(ts)

            self.restart_without_local_files()

            # The persisted page still reflects the key selected before the restart.
            self.validate_latest_kek(ts)
