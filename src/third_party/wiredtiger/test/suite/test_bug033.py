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

import wiredtiger, wttest, threading, wtthread, time
from wiredtiger import stat

# test_bug033.py
# Test for WT-12096.
# Test inserting obsolete updates on the update chain after rolling back to a stable timestamp.
class test_bug033(wttest.WiredTigerTestCase):
    uri = 'table:test_bug033'
    conn_config = 'cache_size=100MB,statistics=(all),timing_stress_for_test=[checkpoint_slow]'

    def evict(self, k):
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(k)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def test_bug033(self):
        # Note, the comments upd_chain: and disk: show what should be
        # the upd_chain and disk at that point.
        self.session.create(self.uri, f'key_format=i,value_format=S')

        # Pin the oldest and stable timestamps to 1
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)},\
                                stable_timestamp={self.timestamp_str(1)}')

        # Create updates at timestamps 2 and 4 that should get blown away by rollback to stable.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        c[0] = 'b'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')
        c.close()
        # upd_chain: 2
        # disk: None
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        c[0] = 'c'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(4)}')
        c.close()
        # upd_chain: 2 -> 4
        # disk: None

        # Evict.
        self.evict(0)
        # upd_chain: Empty
        # disk: 4
        self.conn.rollback_to_stable()
        # upd_chain: tombstone
        # disk: 4

        # Insert another update at timestamp 2.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        c[0] = 'd'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')
        c.close()
        # upd_chain: tombstone -> 2
        # disk: 4

        # Move oldest and stable to 3.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(3)},\
                                stable_timestamp={self.timestamp_str(3)}')
        # upd_chain: tombstone (obsolete) -> 2 (obsolete)
        # disk: 4

        # Give time for the oldest id to update. This ensures the obsolete check removes the
        # obsolete tombstone.
        time.sleep(1)

        # Insert update at timestamp 4.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        c[0] = 'e'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(4)}')
        c.close()
        # upd_chain: 2 (obsolete)
        # disk: 4

        # Create a checkpoint in parallel with the eviction below.
        done = threading.Event()
        ckpt = wtthread.checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start before evicting.
            ckpt_started = 0
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.checkpoint_state][2] != 0
                stat_cursor.close()
                time.sleep(1)

            self.evict(0)
        finally:
            done.set()          # Signal checkpoint to exit.
            ckpt.join()
