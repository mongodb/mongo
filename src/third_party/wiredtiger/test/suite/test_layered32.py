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
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat
import time


# test_layered32.py
# Test that we write internal page deltas to the page log extension.

@disagg_test_class
class test_layered32(wttest.WiredTigerTestCase, DisaggConfigMixin):

    delta = [
        ('write_leaf_only', dict(delta_config='page_delta=(internal_page_delta=false,leaf_page_delta=true)', delta_type='leaf_only')),
        ('write_internal_only', dict(delta_config='page_delta=(internal_page_delta=true,leaf_page_delta=false)', delta_type='internal_only')),
        ('write_none', dict(delta_config='page_delta=(internal_page_delta=false,leaf_page_delta=false)', delta_type='none')),
        ('write_both', dict(delta_config='page_delta=(internal_page_delta=true,leaf_page_delta=true)', delta_type='both')),
    ]

    conn_base_config = 'cache_size=5G,transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),page_delta=(delta_pct=100),'
    disagg_storages = gen_disagg_storages('test_layered32', disagg_only = True)

    nrows = 1000
    uri='file:test_layered32'

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages, delta)

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S,allocation_size=512,leaf_page_max=512,internal_page_max=512,block_manager=disagg'
        return cfg

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="leader"),{self.delta_config},'

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

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
        # Verify the values in the table.
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1, self.nrows + 1):
            cursor.set_key(str(i))
            cursor.search()
            if str(i) in expected_kv:
                self.assertEqual(cursor.get_value(), expected_kv[str(i)])
            else:
                self.assertEqual(cursor.get_value(), expected_initial_val)
        cursor.close()

    def test_internal_page_delta_simple(self):
        self.session.create(self.uri, self.session_create_config())

        # Populate the table with nrows.
        inital_value = "abc" * 10
        inital_ts = 5
        kv = {str(i): inital_value for i in range(1, self.nrows + 1)}
        self.insert(kv, inital_ts)
        self.session.checkpoint()

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Perform two small updates.
        kv_modified = {str(10): "10abc", str(220): "220abc"}
        self.insert(kv_modified, inital_ts + 1)
        # Perform a checkpoint to write out a delta.
        self.session.checkpoint()

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
        self.verify(kv_modified, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

        follower_config = self.conn_base_config + 'disaggregated=(role="follower"),'
        self.reopen_disagg_conn(follower_config)
        time.sleep(1.0)

        # Verify the updated values in the table.
        self.verify(kv_modified, inital_value)

        # Assert that we have constructed at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.cache_read_internal_delta), 0)
        else:
            self.assertEqual(self.get_stat(stat.conn.cache_read_internal_delta), 0)

    def test_internal_page_delta_split_internal(self):

        self.session.create(self.uri, self.session_create_config())

        # Initial population of the table with nrows.
        inital_value = "abc" * 100
        small_value = "b123r" * 20
        inital_ts = 5

        kv = {str(i): small_value for i in range(1, self.nrows + 1)}
        self.insert(kv, inital_ts)

        # First Checkpoint & Reopen to ensure all data is read from the disk.
        self.session.checkpoint()
        self.reopen_disagg_conn(self.conn_config())

        # Updates a sequence of subset of keys with a large value.
        keys_to_update = ["241","242","243", "244", "245", "246", "247", "248", "249", "250"]
        kv_modified = {}
        for key in keys_to_update:
            kv = {key: inital_value}
            inital_ts = inital_ts + 1
            self.insert(kv, inital_ts)

        # Second Checkpoint to force a page split and write the newly split pages to disk.
        self.session.checkpoint()

        # The same set of keys is then updated with a smaller value, which should cause the
        # pages to merge back together and write an internal page delta.
        for key in keys_to_update:
            kv = {key: small_value}
            inital_ts = inital_ts + 1
            self.insert(kv, inital_ts)
            self.session.checkpoint()
            kv_modified.update(kv)

        # Assert that we have written at least one internal page delta.
        if (self.delta_type == 'both' or self.delta_type == 'leaf_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
        if (self.delta_type == 'both' or self.delta_type == 'internal_only'):
            self.assertGreater(self.get_stat(stat.conn.rec_page_delta_internal), 0)
        if (self.delta_type == 'none'):
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_leaf), 0)
            self.assertEqual(self.get_stat(stat.conn.rec_page_delta_internal), 0)

        # Verify the updated values in the table.
        self.verify(kv_modified, small_value)

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Verify the updated values in the table.
        self.verify(kv_modified, small_value)
