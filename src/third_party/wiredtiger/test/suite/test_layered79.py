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

# test_layered79.py
#   Test that when a key is garbage collected during eviction on an ingest
#   btree, its associated on-disk value is also deleted.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered79(wttest.WiredTigerTestCase):
    base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = base_config + 'disaggregated=(role="leader")'
    conn_config_follower = base_config + 'disaggregated=(role="follower")'

    uri = 'layered:test_layered79'
    ingest_uri = 'file:test_layered79.wt_ingest'
    create_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages('test_layered79', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_follow = None
    session_follow = None

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session()

    def test_updated_key_on_disk_value_removed_after_gc(self):
        """
        Scenario:
          ts=10  insert key 1  -> eviction -> key is on disk image
          ts=20  update key 1  -> checkpoint -> update propagated to follower
          advance checkpoint   -> update is prunable
          eviction             -> GC path must write tombstone to clear on-disk entry
          verify               -> key 1 must NOT be found
        """
        self.create_follower()

        self.session.create(self.uri, self.create_config)
        self.session_follow.create(self.uri, self.create_config)

        # --- Step 1: insert key 1 at ts=10, propagate to follower ---
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value1'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor.close()

        # Mirror the insert on the follower ingest btree.
        cursor_follow = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor_follow[1] = 'value1'
        self.session_follow.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor_follow.close()

        # --- Step 2: force eviction of key 1 on the follower to build a disk image ---
        session_evict = self.conn_follow.open_session('debug=(release_evict_page)')
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # --- Step 3: update key 1 at ts=20, propagate to follower ---
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value2'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor.close()

        # Mirror the update on the follower.
        cursor_follow = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor_follow[1] = 'value2'
        self.session_follow.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor_follow.close()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        self.conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # --- Step 4: force eviction of key 1 on the follower ---
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()
        session_evict.close()

        # --- Step 5: verify key 1 is not visible in the ingest btree after GC ---
        cursor_check = self.session_follow.open_cursor(self.ingest_uri)
        cursor_check.set_key(1)
        self.assertEqual(cursor_check.search(), wiredtiger.WT_NOTFOUND)
        cursor_check.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys_update_chain][2]
        self.assertEqual(garbage_collected, 1)

    def test_deleted_key_on_disk_value_removed_after_gc(self):
        """
        Scenario:
          ts=10  insert key 1  -> eviction -> key is on disk image
          ts=20  delete key 1  -> checkpoint -> delete propagated to follower
          advance checkpoint   -> delete is prunable
          eviction             -> GC path must write tombstone to clear on-disk entry
          verify               -> key 1 must NOT be found
        """
        self.create_follower()

        self.session.create(self.uri, self.create_config)
        self.session_follow.create(self.uri, self.create_config)

        # --- Step 1: insert key 1 at ts=10, propagate to follower ---
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value1'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor.close()

        # Mirror the insert on the follower ingest btree.
        cursor_follow = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor_follow[1] = 'value1'
        self.session_follow.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor_follow.close()

        # --- Step 2: force eviction of key 1 on the follower to build a disk image ---
        session_evict = self.conn_follow.open_session('debug=(release_evict_page)')
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # --- Step 3: remove key 1 at ts=20, propagate to follower ---
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor.close()

        # Mirror the remove on the follower.
        cursor_follow = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor_follow.set_key(1)
        cursor_follow.remove()
        self.session_follow.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')
        cursor_follow.close()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        self.conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # --- Step 4: force eviction of key 1 on the follower ---
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()
        session_evict.close()

        # --- Step 5: verify key 1 is not visible in the ingest btree after GC ---
        cursor_check = self.session_follow.open_cursor(self.ingest_uri)
        cursor_check.set_key(1)
        self.assertEqual(cursor_check.search(), wiredtiger.WT_NOTFOUND)
        cursor_check.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys_update_chain][2]
        self.assertEqual(garbage_collected, 1)

    def test_global_visible_tombstone_clears_update_chain_and_on_disk_value(self):
        """
        Scenario:
          ts=10  insert key 1  -> eviction -> key is on disk image
          ts=0   delete key 1
          advance checkpoint   -> insert is not prunable
          eviction             -> GC path must write tombstone to clear on-disk entry
          verify               -> key 1 must NOT be found
        """
        self.create_follower()

        self.session.create(self.uri, self.create_config)
        self.session_follow.create(self.uri, self.create_config)

        # --- Step 1: insert key 1 at ts=10, propagate to follower ---
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'value1'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor.close()

        # Mirror the insert on the follower ingest btree.
        cursor_follow = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor_follow[1] = 'value1'
        self.session_follow.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        cursor_follow.close()

        # --- Step 2: force eviction of key 1 on the follower to build a disk image ---
        session_evict = self.conn_follow.open_session('debug=(release_evict_page)')
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()

        # --- Step 3: remove key 1 at ts=0 on the ingest btree on follower ---
        cursor_follow_ingest = self.session_follow.open_cursor(self.ingest_uri)
        self.session_follow.begin_transaction('no_timestamp=true')
        cursor_follow_ingest.set_key(1)
        cursor_follow_ingest.remove()
        self.session_follow.commit_transaction()
        cursor_follow_ingest.close()

        # Ensure the on-page value is not prnable
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(1)}')
        self.conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(1)}')

        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        # --- Step 4: force eviction of key 1 on the follower ---
        evict_cursor = session_evict.open_cursor(self.uri)
        evict_cursor.set_key(1)
        evict_cursor.search()
        evict_cursor.close()
        session_evict.close()

        # --- Step 5: verify key 1 is not visible in the ingest btree after GC ---
        cursor_check = self.session_follow.open_cursor(self.ingest_uri)
        cursor_check.set_key(1)
        self.assertEqual(cursor_check.search(), wiredtiger.WT_NOTFOUND)
        cursor_check.close()

        stat_cursor = self.session_follow.open_cursor('statistics:' + self.uri)
        garbage_collected = stat_cursor[stat.dsrc.rec_ingest_garbage_collection_keys_update_chain][2]
        self.assertEqual(garbage_collected, 1)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.conn_follow.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
