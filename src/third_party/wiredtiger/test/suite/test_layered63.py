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

import random, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat
import time


# test_layered63.py
# Test that we write internal page deltas.

@disagg_test_class
class test_layered63(wttest.WiredTigerTestCase):

    delta = [
        ('write_leaf_only', dict(delta_config='page_delta=(internal_page_delta=false,leaf_page_delta=true)', delta_type='leaf_only')),
        ('write_internal_only', dict(delta_config='page_delta=(internal_page_delta=true,leaf_page_delta=false)', delta_type='internal_only')),
        ('write_none', dict(delta_config='page_delta=(internal_page_delta=false,leaf_page_delta=false)', delta_type='none')),
        ('write_both', dict(delta_config='page_delta=(internal_page_delta=true,leaf_page_delta=true)', delta_type='both')),
    ]

    conn_base_config = 'cache_size=5G,transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'page_delta=(delta_pct=100),'
    disagg_storages = gen_disagg_storages('test_layered63', disagg_only = True)

    nrows = 1000
    uri='file:test_layered63'

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages, delta)

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S,allocation_size=512,leaf_page_max=512,internal_page_max=512,block_manager=disagg'
        return cfg

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="leader"),{self.delta_config},'

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def insert(self, kv, ts):
        cursor = self.session.open_cursor(self.uri, None, None)
        for k, v in kv.items():
            self.session.begin_transaction()
            cursor[k] = v
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    def verify(self, expected_kv, expected_initial_val):
        # Verify all expected keys and values, including extra inserted keys.
        cursor = self.session.open_cursor(self.uri, None, None)

        # Track the maximum key seen in expected_kv to handle extra inserts.
        all_keys = set(expected_kv.keys())
        max_expected_key = max([int(k) for k in all_keys]) if all_keys else self.nrows

        # Verify base keys (1..nrows)
        for i in range(1, self.nrows + 1):
            key_str = str(i)
            cursor.set_key(key_str)
            if cursor.search() == 0:
                # Key found in base range
                if key_str in expected_kv:
                    self.assertEqual(cursor.get_value(), expected_kv[key_str])
                else:
                    self.assertEqual(cursor.get_value(), expected_initial_val)
            else:
                self.fail(f"Missing expected base key: {key_str}")

        # Verify any extra inserted keys beyond nrows
        for key in range(self.nrows + 1, max_expected_key + 1):
            key_str = str(key)
            self.pr(f"Verifying inserted key: {key_str}")
            if key_str in expected_kv:
                cursor.set_key(key_str)
                ret = cursor.search()
                self.assertEqual(ret, 0, f"Expected inserted key {key_str} not found")
                self.assertEqual(cursor.get_value(), expected_kv[key_str])

        cursor.close()


    def test_internal_page_delta_update(self):
        """
        Scenario:
            Internal page delta updates applied repeatedly on the same set of keys.

        Purpose:
            - Validate that multiple consecutive updates to the same internal page keys
              are correctly recorded as deltas and merged without data loss.
            - Ensure that when a fixed set of keys is updated across several timestamps,
              WiredTiger generates and maintains proper internal and leaf page deltas.
            - Confirm that the latest values for repeatedly updated keys are preserved
              after reopening the connection and performing follower verification.
            - Verify that internal delta pages are constructed and read back successfully.
        """
        self.session.create(self.uri, self.session_create_config())

        # Populate the table with nrows.
        inital_value = "abc" * 10
        inital_ts = 5
        kv = {str(i): inital_value for i in range(1, self.nrows + 1)}
        self.insert(kv, inital_ts)
        self.session.checkpoint()

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        kv_modfied = {}
        num_deltas = random.randint(1, 10)
        # Define a fixed set of 100 keys.
        fixed_keys = [str(k) for k in range(num_deltas, num_deltas + 100)]

        for i in range(1, num_deltas + 1):
            # Each iteration updates the same 10 keys with new values.
            kv = {key: f"value_{i}_{key}" for key in fixed_keys}

            self.insert(kv, inital_ts + i)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(inital_ts + i))
            # Perform a checkpoint to write out a delta.
            self.session.checkpoint()
            # Merge kv into our cumulative dictionary
            kv_modfied.update(kv)

        if (self.delta_type == 'both' or self.delta_type == 'leaf_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_internal), 0)
        if (self.delta_type == 'none'):
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_internal), 0)

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Verify the updated values in the table.
        self.verify(kv_modfied, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

        follower_config = self.conn_base_config + 'disaggregated=(role="follower"),'
        self.reopen_disagg_conn(follower_config)
        time.sleep(1.0)

        # Verify the updated values in the table.
        self.verify(kv_modfied, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

    def test_delta_insert_keys_at_end_of_base_image(self):
        """
        Scenario:
            Delta insert keys at the end of the base image.

        Purpose:
            - Verify that new keys inserted beyond the range of the base image are
              correctly captured and persisted in delta files.
            - Ensure that during merge, these newly inserted keys are properly
              incorporated into the final merged image without overwriting or
              affecting existing base keys.
            - Validate that both internal and leaf page deltas are generated and
              that WiredTiger can successfully read and reconstruct these deltas
              after reopen and follower verification.
            - Confirm correctness when the merge includes both updates to existing
              mid-range keys and inserts of entirely new keys at the end of the keyspace.
        """
        self.session.create(self.uri, self.session_create_config())

        # Populate the table with nrows.
        inital_value = "abc" * 10
        inital_ts = 5
        kv = {str(i): inital_value for i in range(1, self.nrows + 1)}
        self.insert(kv, inital_ts)
        self.session.checkpoint()

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        kv_modified = {}
        num_deltas = random.randint(5, 10)

        # Define a set of keys to insert that are *beyond* the base range.
        # Example: if nrows = 1000, new keys will be 10011010.
        end_insert_keys = [str(self.nrows + k) for k in range(1, 11)]

        for i in range(1, num_deltas + 1):
            # On the last delta, insert new keys at the end of the base image.
            if i == num_deltas:
                kv = {key: f"delta_inserted_value_{i}_{key}" for key in end_insert_keys}
            else:
                # Earlier deltas may optionally update some mid-range base keys.
                kv = {
                    str(random.randint(1, self.nrows // 2)): f"updated_value_{i}_{random.randint(1, 999)}"
                    for _ in range(50)
                }

            self.insert(kv, inital_ts + i)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(inital_ts + i))
            # Perform a checkpoint to write out a delta.
            self.session.checkpoint()
            # Merge kv into our cumulative dictionary.
            kv_modified.update(kv)

        if (self.delta_type == 'both' or self.delta_type == 'leaf_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_internal), 0)
        if (self.delta_type == 'none'):
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_internal), 0)

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Verify the updated and inserted values in the table.
        self.verify(kv_modified, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

        follower_config = self.conn_base_config + 'disaggregated=(role="follower"),'
        self.reopen_disagg_conn(follower_config)
        time.sleep(1.0)

        # Verify the updated and inserted values again from the follower.
        self.verify(kv_modified, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

    def test_base_image_has_more_keys_at_end_of_merge(self):
        """
        Scenario:
            Base image has more keys at the end of the merge.

        Purpose:
            - Validate that when the base image contains additional trailing keys
              beyond those modified in subsequent deltas, the merge process correctly
              retains those untouched base keys.
            - Ensure that deltas only update the intended subset of base keys and that
              unmodified keys from the base image remain intact after all merges.
            - Confirm that internal and/or leaf deltas are created and applied as
              expected.
        """
        self.session.create(self.uri, self.session_create_config())

        # Base image contains more keys than deltas will ever touch.
        extra_base_keys = 50

        # Populate the table with nrows + extra_base_keys in the base image.
        inital_value = "abc" * 10
        inital_ts = 5
        kv = {str(i): inital_value for i in range(1, self.nrows + extra_base_keys + 1)}
        self.insert(kv, inital_ts)
        self.session.checkpoint()

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        kv_modified = {}
        num_deltas = random.randint(2, 5)

        # Define a set of keys within the base range that will be updated in deltas.
        update_keys = [str(random.randint(1, self.nrows)) for _ in range(50)]

        for i in range(1, num_deltas + 1):
            # Each delta updates a subset of base keys (not the trailing ones).
            kv = {key: f"delta_updated_value_{i}_{key}" for key in update_keys}

            self.insert(kv, inital_ts + i)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(inital_ts + i))
            # Perform a checkpoint to write out a delta.
            self.session.checkpoint()
            # Merge kv into our cumulative dictionary.
            kv_modified.update(kv)

        if (self.delta_type == 'both' or self.delta_type == 'leaf_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_internal), 0)
        if (self.delta_type == 'none'):
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_internal), 0)

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Verify both updated keys and unmodified trailing base keys.
        self.verify(kv_modified, inital_value)

        follower_config = self.conn_base_config + 'disaggregated=(role="follower"),'
        self.reopen_disagg_conn(follower_config)
        time.sleep(1.0)

        # Verify both updated keys and unmodified trailing base keys again from the follower.
        self.verify(kv_modified, inital_value)

    def test_internal_page_delta_key_updated_multiple_times(self):
        """
        Scenario:
            We have an internal delta key updated multiple times in consecutive deltas.

        Purpose:
            - Ensure that when the same internal key (spanning internal boundaries)
              is updated repeatedly across multiple deltas, WiredTiger correctly
              merges and applies the latest value.
            - Validate that internal deltas are created and read back successfully.
        """

        self.session.create(self.uri, self.session_create_config())
        # Populate the table with nrows of base data.
        initial_value = "abc" * 10
        initial_ts = 5
        kv = {str(i): initial_value for i in range(1, self.nrows + 1)}
        self.insert(kv, initial_ts)
        self.session.checkpoint()

        # Re-open the connection to clear cache and simulate disaggregated state.
        self.reopen_disagg_conn(self.conn_config())

        kv_modified = {}
        num_deltas = 8  # Fixed number of deltas for determinism

        # Pick keys that will be spread across multiple leaf pages.
        # This ensures updates go beyond a single leaf and trigger internal reconciliation.
        internal_boundary_keys = [
            str(self.nrows // 4 + i) for i in range(10)
        ] + [
            str(self.nrows // 2 + i) for i in range(10)
        ] + [
            str(3 * self.nrows // 4 + i) for i in range(10)
        ]

        self.pr(f"Internal key set repeatedly updated: {internal_boundary_keys}")

        for i in range(1, num_deltas + 1):
            # Update the same internal keys in each delta.
            kv_updates = {key: f"update_{i}_{key}" for key in internal_boundary_keys}

            # Also update a small random subset of other keys to force internal reconciliation.
            for j in range(20):
                rand_key = str(random.randint(1, self.nrows))
                kv_updates[rand_key] = f"noise_{i}_{rand_key}"

            self.insert(kv_updates, initial_ts + i)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(initial_ts + i))

            # Force a delta checkpoint.
            self.session.checkpoint("use_timestamp=true,force=true")
            kv_modified.update(kv_updates)

        # ---- Assert deltas were generated ----
        leaf_deltas = self.get_stat(stat.conn.rec_page_delta_leaf)
        internal_deltas = self.get_stat(stat.conn.rec_page_delta_internal)

        self.pr(f"Leaf deltas recorded: {leaf_deltas}, internal deltas recorded: {internal_deltas}")

        if self.delta_type in ('both', 'leaf_only'):
            self.assertGreater(leaf_deltas, 0, "Expected at least one leaf page delta")

        if self.delta_type in ('both', 'internal_only'):
            self.assertGreater(internal_deltas, 0, "Expected at least one internal page delta")

        if self.delta_type == 'none':
            self.assertEqual(leaf_deltas, 0)
            self.assertEqual(internal_deltas, 0)

        # ---- Validate after reopening ----
        self.reopen_disagg_conn(self.conn_config())
        self.verify(kv_modified, initial_value)

        read_internal = self.get_stat(stat.conn.cache_read_internal_delta)
        self.pr(f"Internal delta reads: {read_internal}")

        if self.delta_type in ('both', 'internal_only'):
            self.assertGreater(read_internal, 0, "Expected to read at least one internal delta")
        else:
            self.assertEqual(read_internal, 0)

        # ---- Follower check ----
        follower_config = self.conn_base_config + 'disaggregated=(role="follower"),'
        self.reopen_disagg_conn(follower_config)
        time.sleep(1.0)

        self.verify(kv_modified, initial_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)
