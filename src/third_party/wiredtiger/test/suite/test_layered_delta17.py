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

import wttest, wiredtiger
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_delta17.py
#
# Reconstructing a leaf page from its base image and deltas drops every key whose
# stop is globally visible. When all of a page's keys are dropped, the merge
# produces a leaf image with no entries: check the reader tolerates that empty
# reconstructed page.
@disagg_test_class
class test_layered_delta17(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f"layered:{test_name}"

    conn_base_config = 'statistics=(all),transaction_sync=(enabled,method=fsync),' \
                     + 'page_delta=(delta_pct=100,leaf_page_delta=true),precise_checkpoint=true,'
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 10

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def session_create_config(self):
        return 'key_format=S,value_format=S'

    def evict(self, key):
        # Force the page holding key out of cache so the next access rebuilds it
        # from its base image and deltas.
        s = self.conn.open_session("debug=(release_evict_page)")
        c = s.open_cursor(self.uri, None, None)
        s.begin_transaction()
        c.set_key(key)
        c.search_near()
        c.close()
        s.rollback_transaction()
        s.close()

    def test_empty_reconstructed_page(self):
        self.session.create(self.uri, self.session_create_config())
        value = "a" * 20

        # Populate the leaf, delete every key, and checkpoint with the oldest
        # timestamp still behind the delete. The tombstones are not yet globally
        # visible, so they are retained and written into the leaf's base image
        # (rather than the page being collapsed away).
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(5)}')
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.evict(str(0))

        # Add and then remove a throwaway key, checkpointing each change so the
        # page carries leaf deltas on top of the tombstone-bearing base image:
        # reconstructing it now has to run the base+delta merge. The throwaway
        # key nets out to nothing, and the oldest timestamp is still behind every
        # delete so nothing is dropped at write time.
        self.session.begin_transaction()
        cursor['zzz'] = value
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(12)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(12))
        self.session.checkpoint()
        self.assertGreaterEqual(self.get_stat(stat.dsrc.rec_page_delta_leaf, self.uri), 1)
        self.session.begin_transaction()
        cursor.set_key('zzz')
        cursor.remove()
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(14)}')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(14))
        self.session.checkpoint()
        self.evict(str(0))

        # Make every delete globally visible, then read the page back. The read
        # rebuilds it from the base image and deltas; every key's stop is now
        # globally visible, so the merge drops them all and produces an empty leaf.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(20)},'
                                f'stable_timestamp={self.timestamp_str(20)}')

        cursor.close()
        cursor = self.session.open_cursor(self.uri, None, None)

        # Forward and backward scans see nothing.
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.prev(), wiredtiger.WT_NOTFOUND)

        # A point search for any prior key returns not-found rather than crashing.
        for i in range(self.nitems):
            cursor.set_key(str(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        # The empty reconstructed page verifies and checkpoints cleanly.
        self.session.verify(self.uri, None)
        self.session.checkpoint()
