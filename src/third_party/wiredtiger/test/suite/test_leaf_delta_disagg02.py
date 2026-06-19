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
import wiredtiger
from helpers.helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# Test that reconciliation writes a full page instead of a delta when too many keys have
# been removed from the disk image. A removed key still occupies space as a tombstone in
# the delta; the same key is simply absent from a full page. The threshold is controlled
# by page_delta.delete_pct.
@disagg_test_class
class test_leaf_delta_disagg02(wttest.WiredTigerTestCase):

    test_name = __qualname__
    uri = f'layered:{test_name}'

    # Use delta_pct=1000 so the size threshold never rejects a delta; only the delete
    # threshold should determine whether we write a delta or a full page.
    # Use delete_pct=50 explicitly to test the boundary at a fixed, predictable value.
    conn_base_config = (
        'cache_size=32MB,'
        'transaction_sync=(enabled,method=fsync),'
        'statistics=(all),'
        'statistics_log=(wait=1,json=true,on_close=true),'
        'page_delta=(delta_pct=1000,delete_pct=50),'
    )
    conn_delta_config = 'disaggregated=(role="leader"),page_delta=(leaf_page_delta=true,internal_page_delta=false),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Small pages ensure all keys land on a single leaf page.
    TABLE_CONFIG = (
        'key_format=S,value_format=S,'
        'allocation_size=512,leaf_page_max=4096,'
        'block_manager=disagg'
    )
    NKEYS = 10

    def conn_config(self):
        return self.conn_base_config + self.conn_delta_config

    def get_stat(self, stat_key, uri=None):
        target = f'statistics:{uri}' if uri else 'statistics:'
        cursor = self.session.open_cursor(target, None, None)
        val = cursor[stat_key][2]
        cursor.close()
        return val

    def make_key(self, i):
        # Zero-pad so all keys are the same width, helping prefix compression produce
        # a deterministic page layout.
        return f'key{i:04d}'

    def populate(self, ts):
        self.session.create(self.uri, self.TABLE_CONFIG)
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.NKEYS):
            self.session.begin_transaction()
            cursor[self.make_key(i)] = f'value{i:04d}'
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()
        self.conn.set_timestamp(
            f'oldest_timestamp={self.timestamp_str(ts)},stable_timestamp={self.timestamp_str(ts)}'
        )
        self.session.checkpoint()

    def delete_keys(self, indices, ts):
        cursor = self.session.open_cursor(self.uri)
        for i in indices:
            self.session.begin_transaction()
            cursor.set_key(self.make_key(i))
            cursor.remove()
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def update_keys(self, indices, new_value, ts):
        cursor = self.session.open_cursor(self.uri)
        for i in indices:
            self.session.begin_transaction()
            cursor[self.make_key(i)] = new_value
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def verify_present(self, indices, ts):
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
        cursor = self.session.open_cursor(self.uri)
        for i in indices:
            cursor.set_key(self.make_key(i))
            self.assertEqual(cursor.search(), 0, f'expected key {i} to be present')
        cursor.close()
        self.session.rollback_transaction()

    def verify_absent(self, indices, ts):
        self.session.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
        cursor = self.session.open_cursor(self.uri)
        for i in indices:
            cursor.set_key(self.make_key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND,
                             f'expected key {i} to be absent')
        cursor.close()
        self.session.rollback_transaction()

    def test_delete_threshold_exceeded(self):
        """
        Remove 7 of 10 keys from the disk image (70% of the page), above the 50%
        delete_pct threshold. Reconciliation must write a full page, not a delta.
        """
        base_ts = 10
        self.populate(base_ts)

        self.reopen_disagg_conn(self.conn_config())

        # Delete indices 0-6 (7 out of 10 keys = 70% of page entries).
        delete_ts = 20
        deleted = list(range(7))
        surviving = list(range(7, self.NKEYS))
        self.delete_keys(deleted, delete_ts)
        self.conn.set_timestamp(
            f'oldest_timestamp={self.timestamp_str(delete_ts)},'
            f'stable_timestamp={self.timestamp_str(delete_ts)}'
        )
        self.session.checkpoint()

        # Threshold fired: too many keys removed from disk image, full page written.
        self.assertEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 0)
        self.assertGreater(
            self.get_stat(stat.dsrc.rec_page_delta_rejected_delete_threshold, self.uri), 0
        )

        # Data must be correct after the full-page write.
        self.verify_absent(deleted, delete_ts)
        self.verify_present(surviving, delete_ts)

        # Re-open and verify data survives across a connection restart.
        self.reopen_disagg_conn(self.conn_config())
        self.verify_absent(deleted, delete_ts)
        self.verify_present(surviving, delete_ts)

    def test_delete_threshold_not_exceeded(self):
        """
        Remove 3 of 10 keys from the disk image (30% of the page) and update 4 others,
        keeping the removed fraction below the 50% delete_pct threshold.
        Reconciliation must write a delta.
        """
        base_ts = 10
        self.populate(base_ts)

        self.reopen_disagg_conn(self.conn_config())

        # Delete indices 0-2 (3 out of 10 keys = 30%) and update indices 3-6 (4 keys).
        mod_ts = 20
        deleted = list(range(3))
        updated = list(range(3, 7))
        surviving = list(range(3, self.NKEYS))

        self.delete_keys(deleted, mod_ts)
        self.update_keys(updated, 'updated_value', mod_ts)
        self.conn.set_timestamp(
            f'oldest_timestamp={self.timestamp_str(mod_ts)},'
            f'stable_timestamp={self.timestamp_str(mod_ts)}'
        )
        self.session.checkpoint()

        # Removed fraction (3/10 = 30%) is below the threshold: a delta must be written.
        self.assertGreater(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 0)
        self.assertEqual(
            self.get_stat(stat.dsrc.rec_page_delta_rejected_delete_threshold, self.uri), 0
        )

        # Data must be correct after the delta write.
        self.verify_absent(deleted, mod_ts)
        self.verify_present(surviving, mod_ts)

        # Re-open and verify data survives across a connection restart.
        self.reopen_disagg_conn(self.conn_config())
        self.verify_absent(deleted, mod_ts)
        self.verify_present(surviving, mod_ts)
