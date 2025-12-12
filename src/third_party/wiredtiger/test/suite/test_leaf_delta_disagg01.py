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
from wtscenario import make_scenarios
from wiredtiger import stat
import wiredtiger

# test_leaf_delta_disagg01.py
# Test we can build leaf delta disk image from base image and deltas correctly, the test covers
# different scenarios, where the k/v pair on latest delta should overwrite the same k/v pair for
# earlier delta, the unpacking during merging process for delta and base image should work properly.
@disagg_test_class
class test_leaf_delta_disagg01(wttest.WiredTigerTestCase):
    prefix_compression = [
        ('enabled', dict(prefix_config='prefix_compression=true', prefix_enabled=True)),
        ('disabled', dict(prefix_config='prefix_compression=false', prefix_enabled=False)),
    ]
    conn_base_config = 'cache_size=32MB,transaction_sync=(enabled,method=fsync),statistics=(all),' \
    'statistics_log=(wait=1,json=true,on_close=true),page_delta=(delta_pct=100),'
    conn_delta_config = 'disaggregated=(role="leader"),page_delta=(internal_page_delta=true,leaf_page_delta=true),'
    disagg_storages = gen_disagg_storages('test_layered54', disagg_only = True)

    uri='layered:test_leaf_delta_disagg01'
    init_key = "abc"

    scenarios = make_scenarios(disagg_storages, prefix_compression)

    base_ids = []
    delta1_ids = []
    delta2_ids = []
    delta3_ids = []
    base_vals = []
    delta1_vals = []
    delta2_vals = []
    delta3_vals = []

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=u,allocation_size=512,leaf_page_max=512,' \
        'internal_page_max=512,block_manager=disagg,'
        return cfg

    def conn_config(self):
        return self.conn_base_config + self.conn_delta_config

    def get_stat(self, stat, uri = None):
        if not uri:
            uri = ''
        stat_cursor = self.session.open_cursor(f'statistics:{uri}', None, None)
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def insert_or_update(self, ids, vals):
        cursor = self.session.open_cursor(self.uri, None, None)
        for id, val in zip(ids, vals):
            cursor[self.init_key * id] = val.encode()
        cursor.close()

    def verify(self, dict):
        cursor = self.session.open_cursor(self.uri, None, None)
        for id, val in dict.items():
            cursor.set_key(self.init_key * id)
            cursor.search()
            self.assertEqual(cursor.get_value(), val.encode())
        cursor.close()

    def delete(self, ids):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in ids:
            cursor.set_key(self.init_key * i)
            cursor.remove()
        cursor.close()
        self.session.checkpoint()

    def verify_delete(self, ids):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in ids:
            cursor.set_key(self.init_key * i)
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def verify_leaf_delta(self):
        self.session.create(self.uri, self.session_create_config() + self.prefix_config)

        delta_cnt = 0
        # Populate the table.
        self.insert_or_update(self.base_ids, self.base_vals)
        self.session.checkpoint()
        if (self.prefix_enabled):
            self.assertGreater(self.get_stat(stat.dsrc.rec_prefix_compression_full, self.uri), 0)
        else:
            self.assertEqual(self.get_stat(stat.dsrc.rec_prefix_compression_full, self.uri), 0)
        self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 0)

        # Perform updates to generate 3 deltas with overlapping keys.
        self.reopen_disagg_conn(self.conn_config())
        self.insert_or_update(self.delta1_ids, self.delta1_vals)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        self.reopen_disagg_conn(self.conn_config())
        self.insert_or_update(self.delta2_ids, self.delta2_vals)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        self.reopen_disagg_conn(self.conn_config())
        self.insert_or_update(self.delta3_ids, self.delta3_vals)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        # There should be 3 deltas generated for the page.
        self.assertEqual(delta_cnt, 3)
        if (self.prefix_enabled):
            self.assertGreater(self.get_stat(stat.dsrc.rec_prefix_compression_delta, self.uri), 0)
        else:
            self.assertEqual(self.get_stat(stat.dsrc.rec_prefix_compression_delta, self.uri), 0)

        self.reopen_disagg_conn(self.conn_config())
        dict_b = dict(zip(self.base_ids, self.base_vals))
        dict1 = dict(zip(self.delta1_ids, self.delta1_vals))
        dict2 = dict(zip(self.delta2_ids, self.delta2_vals))
        dict3 = dict(zip(self.delta3_ids, self.delta3_vals))

        # Since deltas are generated in time order, the keys in delta3_ids are the latest update so
        # values are from delta3, and keys included in delta2_ids but excluded from delta3_ids are
        # having values from delta2, and so on.
        self.verify(dict3)
        diff = {k: v for k, v in dict2.items() if k not in dict3}
        self.verify(diff)
        diff = {k: v for k, v in dict1.items() if k not in dict2}
        diff = {k: v for k, v in diff.items() if k not in dict3}
        self.verify(diff)
        diff = {k: v for k, v in dict_b.items() if k not in dict1}
        diff = {k: v for k, v in diff.items() if k not in dict2}
        diff = {k: v for k, v in diff.items() if k not in dict3}
        self.verify(diff)

    # Test deltas with no duplicate keys among them, the delta keys should only overwrite the keys
    # on base image.
    def test_delta_no_duplicate_keys(self):
        self.base_ids = [i for i in range(1, 11)]
        self.delta1_ids = [1, 2, 3]
        self.delta2_ids = [4, 5, 6]
        self.delta3_ids = [7, 8, 9]
        self.base_vals = ["base"] * 10
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

    # Test deltas having duplicate keys, the latest delta should overwrite all ealier deltas for a
    # given key.
    def test_delta_duplicate_keys(self):
        self.base_ids = [i for i in range(1, 11)]
        self.delta1_ids = [1, 2, 3, 4, 8]
        self.delta2_ids = [3, 4, 9]
        self.delta3_ids = [6, 8, 10]
        self.base_vals = ["base"] * 10
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

    # Test deltas with some newly inserted keys.
    def test_delta_inserted_keys(self):
        self.base_ids = list({i for i in range(3, 13)} - {4, 7, 8})
        self.delta1_ids = [1, 4, 6]
        self.delta2_ids = [2, 6, 7]
        self.delta3_ids = [6, 8, 10, 15]
        self.base_vals = ["base"] * 10
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

    # Test base image having empty values for all entries.
    def test_base_empty_values_all(self):
        self.base_ids = [i for i in range(1, 11)]
        self.delta1_ids = [1, 2, 3, 4, 8]
        self.delta2_ids = [3, 4, 9]
        self.delta3_ids = [6, 8, 10]
        self.base_vals = [""] * 10
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

    # Test base image having mixed empty and non-empty values.
    def test_base_empty_values_mixed(self):
        self.base_ids = [i for i in range(1, 11)]
        self.delta1_ids = [1, 2, 3, 4, 8]
        self.delta2_ids = [3, 4, 9]
        self.delta3_ids = [6, 8, 10]
        self.base_vals = ["base", "", "base"] * 3 + [""]
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

    # Test mixed of empty/non-empty values, inserted keys, duplicate keys among base image and deltas.
    def test_comprehensive(self):
        self.base_ids = list({i for i in range(1, 11)} - {4, 5, 6})
        self.delta1_ids = [1, 2, 3, 4, 8]
        self.delta2_ids = [3, 4, 9]
        self.delta3_ids = [6, 8, 10, 12, 17]
        self.base_vals = ["", "base", ""] + ["base"] * 4
        self.delta1_vals = ["d1", "", ""]
        self.delta2_vals = ["d2", "", "d2"]
        self.delta3_vals = ["", "d3", "d3"]
        self.verify_leaf_delta()

    # Test delete of keys.
    def test_delete(self):
        self.base_ids = [i for i in range(1, 11)]
        self.delta1_ids = [1, 2, 3, 4, 8]
        self.delta2_ids = [3, 4, 9]
        self.delta3_ids = [6, 8, 10]
        self.base_vals = ["base", ""] * 5
        self.delta1_vals = ["d1"] * 3
        self.delta2_vals = ["d2"] * 3
        self.delta3_vals = ["d3"] * 3
        self.verify_leaf_delta()

        delete_ids = [1, 3, 10]
        self.reopen_disagg_conn(self.conn_config())
        self.delete(delete_ids)
        # There should be 1 delta with deleted keys generated for the page.
        self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 1)

        self.reopen_disagg_conn(self.conn_config())
        self.verify_delete(delete_ids)
