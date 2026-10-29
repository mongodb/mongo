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

# Regression test: a follower's stable-checkpoint dhandle open races with a checkpoint pickup's
# ingest prune walk.
#
# Regression: the pickup sets WT_DHANDLE_OUTDATED on the superseded dhandle, then reads
# session_inuse to decide how far to prune the ingest table. The reader tested OUTDATED during the
# dhandle lookup, before the open published it in session_inuse, so neither side need observe the
# other: the reader binds a checkpoint whose ingest content is then pruned, returning a stale value.
#
# This test suspends the reader inside the dhandle open so the entire pickup and prune walk complete
# before the reader resumes; without the fix the reader binds the superseded checkpoint and reads a
# stale value after the ingest content is evicted.

import threading
import time
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
import wiredtiger

@disagg_test_class
class test_disagg_bug01(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    ingest_uri = f'file:{test_name}.wt_ingest'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,cache_size=512MB,statistics=(all),'
    nitems = 1000

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
        return self.extensionsConfig() + self.conn_base_config + \
            'disaggregated=(role="follower")'

    def get_stat(self, session, stat_key):
        stat_cursor = session.open_cursor('statistics:' + self.uri)
        stat_cursor.set_key(stat_key)
        stat_cursor.search()
        val = stat_cursor.get_value()[2]
        stat_cursor.close()
        return val

    def add_data(self, session, value, ts):
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        for i in range(self.nitems):
            cursor[f'key{i:06d}'] = value
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def test_follower_read_survives_checkpoint_pickup_during_stable_open(self):
        self.session.create(self.uri, self.table_config)

        # Insert v1 at ts 10, sealed into checkpoint N.
        self.add_data(self.session, 'v1', 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        # Open a follower connection and deliver checkpoint N to it.
        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        self.disagg_advance_checkpoint_and_wait(conn_follow)

        # open and close a cursor on it so the checkpoint's stable dhandle is warm in the
        # connection's shared handle list. The reader thread below opens its own session with an
        # empty per-session dhandle cache, so it must resolve the handle through the shared-handle
        # path where the stress point lives.
        warm_cursor = session_follow.open_cursor(self.uri)
        self.assertEqual(warm_cursor.next(), 0)
        warm_cursor.close()

        # Insert v2 at ts 20, sealed into checkpoint N+1 on the leader (but not the follower).
        self.add_data(self.session, 'v2', 20)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20) +
            ',oldest_timestamp=' + self.timestamp_str(15))
        self.session.checkpoint()

        reader_started = threading.Event()
        result = {}

        def reader():
            s = conn_follow.open_session('')
            s.begin_transaction('read_timestamp=' + self.timestamp_str(20))
            # Signal before opening the cursor: the main thread delivers the pickup while the
            # stress point suspends this open inside __wt_session_get_dhandle.
            reader_started.set()
            c = s.open_cursor(self.uri)
            c.set_key('key000500')
            if c.search() == 0:
                result['value'] = c.get_value()
            else:
                result['value'] = 'notfound'
            c.close()
            s.rollback_transaction()
            s.close()

        reader_thread = threading.Thread(target=reader)

        # Arm the stress point on the follower connection. The stress point is scoped to stable
        # URIs, so it delays only the reader's stable-dhandle open, not the pickup's own
        # ingest-table dhandle lookups.
        conn_follow.reconfigure('timing_stress_for_test=[disagg_stable_dhandle_delay]')
        reader_thread.start()
        self.assertTrue(reader_started.wait(10), 'reader thread did not start in time')

        # Let the reader reach the sleep inside the stable dhandle open.
        time.sleep(0.5)

        # Deliver and adopt checkpoint N+1 while the reader is suspended: the merge marks the
        # stable dhandle OUTDATED and the prune walk advances the ingest prune timestamp past
        # checkpoint N (the dhandle is idle: session_inuse == 0).
        self.disagg_advance_checkpoint_and_wait(conn_follow)
        conn_follow.reconfigure('timing_stress_for_test=[]')

        # Force eviction of the follower's ingest pages so entries at or below the prune timestamp
        # are dropped from the ingest table.
        evict_cursor = session_follow.open_cursor(self.ingest_uri, None, 'debug=(release_evict)')
        for i in range(self.nitems):
            evict_cursor.set_key(f'key{i:06d}')
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()

        reader_thread.join(30)
        self.assertFalse(reader_thread.is_alive(),
            'reader thread did not finish within the timeout')

        # Prove the reader actually hit the retry path: without it, a reader that reaches the
        # sleep too late to race the pickup would resolve checkpoint N+1 directly and the test
        # would pass without exercising the fix.
        self.assertGreater(
            self.get_stat(session_follow, wiredtiger.stat.dsrc.layered_curs_open_stable_ckpt_pickup_race), 0,
            'reader did not race the checkpoint pickup')

        # At read_timestamp 20 the correct value is v2. v1 means the reader bound to checkpoint N
        # after the ingest content covering it had already been pruned.
        self.assertEqual(result.get('value'), 'v2',
            f'reader saw a stale value: {result.get("value")}')

        session_follow.close()
        conn_follow.close()
