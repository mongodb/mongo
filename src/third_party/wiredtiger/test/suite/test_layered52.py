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

import random, wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat
import time


# test_layered51.py
# Test that we write internal page deltas with deleted leaf page
# to the page log extension.

@disagg_test_class
class test_layered52(wttest.WiredTigerTestCase):

    uri = 'file:test_layered52'

    conn_base_config = 'transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'page_delta=(delta_pct=100),'
    disagg_storages = gen_disagg_storages('test_layered52', disagg_only = True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages)

    def session_create_config(self):
        # The delta percentage of 100 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S,allocation_size=512,leaf_page_max=512,internal_page_max=512,block_manager=disagg'
        return cfg

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="leader"),page_delta=(internal_page_delta=true,leaf_page_delta=false),'

    def verify_stat(self):
        # Assert that we have deleted at least one internal key page delta.
        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertGreater(stat_cursor[stat.dsrc.rec_page_delta_internal_key_deleted][2], 0)
        stat_cursor.close()

        # Assert that we have written at least one internal page delta.
        stat_cursor = self.session.open_cursor('statistics:' + self.uri)
        self.assertGreater(stat_cursor[stat.dsrc.rec_page_delta_internal][2], 0)
        stat_cursor.close()

    def insert(self, kv, ts):
        cursor = self.session.open_cursor(self.uri, None, None)
        for k, v in kv.items():
            self.session.begin_transaction()
            cursor[k] = v
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    def delete_keys(self, keys, ts):
        cursor = self.session.open_cursor(self.uri, None, None)
        for k in keys:
            self.session.begin_transaction()
            cursor.set_key(k)
            cursor.remove()
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    def verify(self, expected_keys, ts):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(1, self.nitems + 1):
            key_str = str(i)
            cursor.set_key(key_str)
            if key_str in expected_keys:
                self.assertNotEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    # Use more items so we reliably get a deeper tree and enough internal keys.
    nitems = 5000

    def test_internal_page_delta_delete_leaf(self):

        self.session.create(self.uri, self.session_create_config())

        # Populate the table with nitems.
        initial_value = "abc" * 10
        initial_ts = 5
        kv = {str(i): initial_value for i in range(1, self.nitems + 1)}
        self.insert(kv, initial_ts)
        self.session.checkpoint()

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        expected_keys = set(str(i) for i in range(1, self.nitems + 1))

        #
        # Drive multiple substantial changes into the internal tree so that
        # reconciliation has more than one opportunity to write internal
        # page deltas with deleted children.
        #

        # First delete range: a contiguous block of keys somewhere in the
        # middle of the key space. Use a range large enough to span several
        # leaf pages given the small page sizes.
        delete_start1 = 200
        delete_len1 = 200
        keys_to_delete1 = [
            str(k)
            for k in range(delete_start1, delete_start1 + delete_len1)
            if str(k) in expected_keys
        ]

        delete_ts1 = initial_ts + 10
        self.delete_keys(keys_to_delete1, delete_ts1)

        self.conn.set_timestamp('oldest_timestamp={},stable_timestamp={}'.format(
            self.timestamp_str(delete_ts1), self.timestamp_str(delete_ts1)))

        # First checkpoint after the first delete range.
        self.session.checkpoint()
        expected_keys.difference_update(keys_to_delete1)

        # Second delete range: a separate block of keys further along the key
        # space to affect different leaves and internal children.
        delete_start2 = 3000
        delete_len2 = 200
        keys_to_delete2 = [
            str(k)
            for k in range(delete_start2, delete_start2 + delete_len2)
            if str(k) in expected_keys
        ]

        delete_ts2 = delete_ts1 + 10
        self.delete_keys(keys_to_delete2, delete_ts2)

        self.conn.set_timestamp('oldest_timestamp={},stable_timestamp={}'.format(
            self.timestamp_str(delete_ts2), self.timestamp_str(delete_ts2)))

        # Second checkpoint after the second delete range. Between the two
        # checkpoints, internal reconciliation should see multiple children
        # becoming deletable and is more likely to write internal deltas
        # that include deleted keys.
        self.session.checkpoint()

        expected_keys.difference_update(keys_to_delete2)

        # After two delete+checkpoint sequences on disjoint ranges, we should
        # now have internal page deltas that include deleted keys.
        self.verify_stat()

        # Verify that only the expected keys are present at the latest
        # stable timestamp.
        self.verify(expected_keys, delete_ts2)

        # Re-open the connection to clear contents out of memory.
        self.reopen_disagg_conn(self.conn_config())

        # Verify the updated values in the table.
        self.verify(expected_keys, delete_ts2)
