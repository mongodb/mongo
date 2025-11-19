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

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S,allocation_size=512,leaf_page_max=512,' \
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

    def insert_or_update(self, empty_score, ids, value, ts):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in ids:
            self.session.begin_transaction()
            cursor[self.init_key * i] = "" if i % empty_score == 0 else value
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    def verify(self, empty_score, ids, value):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in ids:
            cursor.set_key(self.init_key * i)
            cursor.search()
            self.assertEqual(cursor.get_value(), "" if i % empty_score == 0 else value)
        cursor.close()

    # xx_empty_score is a factor to determine whether a value should be set to empty. For a given
    # index i, if i % xx_empty_score == 0, that entry is set to empty. If xx_empty_score is 1 then
    # all entries will have empty values.
    def verify_leaf_delta(self, base_empty_score, delta_empty_score, base_ids, delta1_ids = None,
                          delta2_ids = None, delta3_ids = None):
        if delta1_ids is None:
            delta1_ids = {}
        if delta2_ids is None:
            delta2_ids = {}
        if delta3_ids is None:
            delta3_ids = {}
        self.session.create(self.uri, self.session_create_config() + self.prefix_config)

        delta_cnt = 0
        # Populate the table.
        base_value = "base"
        ts = 1
        self.insert_or_update(base_empty_score, base_ids, base_value, ts)
        self.session.checkpoint()
        if (self.prefix_enabled):
            self.assertGreater(self.get_stat(stat.dsrc.rec_prefix_compression_full, self.uri), 0)
        else:
            self.assertEqual(self.get_stat(stat.dsrc.rec_prefix_compression_full, self.uri), 0)
        self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 0)

        # Perform updates to generate 3 deltas with overlapping keys.
        self.reopen_disagg_conn(self.conn_config())
        ts += 1
        delta1_value = "delta1"
        self.insert_or_update(delta_empty_score, delta1_ids, delta1_value, ts)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        self.reopen_disagg_conn(self.conn_config())
        ts += 1
        delta2_value = "delta2"
        self.insert_or_update(delta_empty_score, delta2_ids, delta2_value, ts)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        self.reopen_disagg_conn(self.conn_config())
        ts += 1
        delta3_value = "delta3"
        self.insert_or_update(delta_empty_score, delta3_ids, delta3_value, ts)
        self.session.checkpoint()
        delta_cnt += self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri)

        # There should be 3 deltas generated for the page.
        self.assertEqual(delta_cnt, 3)
        if (self.prefix_enabled):
            self.assertGreater(self.get_stat(stat.dsrc.rec_prefix_compression_delta, self.uri), 0)
        else:
            self.assertEqual(self.get_stat(stat.dsrc.rec_prefix_compression_delta, self.uri), 0)

        # Since deltas are generated in time order, the keys in delta3_ids are the latest update so
        # they have value 'delta3', and keys included in delta2_ids but excluded from delta3_ids are
        # having value 'delta2', and so on.
        self.reopen_disagg_conn(self.conn_config())
        self.verify(delta_empty_score, delta3_ids, delta3_value)
        self.verify(delta_empty_score, delta2_ids - delta3_ids, delta2_value)
        self.verify(delta_empty_score, delta1_ids - delta2_ids - delta3_ids, delta1_value)
        self.verify(base_empty_score, base_ids - delta1_ids - delta2_ids - delta3_ids, base_value)

    # Test deltas with no duplicate keys among them, the delta keys should only overwrite the keys
    # on base image.
    def test_delta_no_duplicate_keys(self):
        base_ids = {i for i in range(1, 11)}
        delta1_ids = {1, 2, 3}
        delta2_ids = {4, 5, 6}
        delta3_ids = {7, 8, 9}
        self.verify_leaf_delta(1e9, 1e9, base_ids, delta1_ids, delta2_ids, delta3_ids)

    # Test deltas having duplicate keys, the latest delta should overwrite all ealier deltas for a
    # given key.
    def test_delta_duplicate_keys(self):
        base_ids = {i for i in range(1, 11)}
        delta1_ids = {1, 2, 3, 4, 8}
        delta2_ids = {3, 4, 9}
        delta3_ids = {6, 8, 10}
        self.verify_leaf_delta(1e9, 1e9, base_ids, delta1_ids, delta2_ids, delta3_ids)

    # Test deltas with some newly inserted keys.
    def test_delta_inserted_keys(self):
        base_ids = {i for i in range(3, 13)} - {4, 7, 8}
        delta1_ids = {1, 4, 6}
        delta2_ids = {2, 7, 6}
        delta3_ids = {6, 8, 10, 15}
        self.verify_leaf_delta(1e9, 1e9, base_ids, delta1_ids, delta2_ids, delta3_ids)

    # Test base image having empty values for all entries.
    def test_base_empty_values_all(self):
        base_ids = {i for i in range(1, 11)}
        delta1_ids = {1, 2, 3, 4, 8}
        delta2_ids = {3, 4, 9}
        delta3_ids = {6, 8, 10}
        self.verify_leaf_delta(1, 1e9, base_ids, delta1_ids, delta2_ids, delta3_ids)

    # Test base image having mixed empty and non-empty values.
    def test_base_empty_values_mixed(self):
        base_ids = {i for i in range(1, 11)}
        delta1_ids = {1, 2, 3, 4, 8}
        delta2_ids = {3, 4, 9}
        delta3_ids = {6, 8, 10}
        self.verify_leaf_delta(2, 1e9, base_ids, delta1_ids, delta2_ids, delta3_ids)

    # Test mixed of empty/non-empty values, inserted keys, duplicate keys among base image and deltas.
    def test_base_empty_values_mixed(self):
        base_ids = {i for i in range(1, 11)} - {4, 5, 6}
        delta1_ids = {1, 2, 3, 4, 8}
        delta2_ids = {3, 4, 9}
        delta3_ids = {6, 8, 10, 12, 17}
        self.verify_leaf_delta(2, 2, base_ids, delta1_ids, delta2_ids, delta3_ids)
