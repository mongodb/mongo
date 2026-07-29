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

# The follower's stable cursor lifecycle for reads. An exact search defers the stable open until the
# ingest lookup misses; search_near merges the constituents, so it always opens stable. Once open,
# the stable cursor must not be reopened until a newer checkpoint arrives.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_follower17(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")'

    def get_stat(self, session, stat_key):
        stat_cursor = session.open_cursor('statistics:')
        stat_cursor.set_key(stat_key)
        stat_cursor.search()
        val = stat_cursor.get_value()[2]
        stat_cursor.close()
        return val

    def opened(self, session):
        return self.get_stat(session, wiredtiger.stat.conn.layered_curs_open_stable)

    def reopened(self, session):
        return self.get_stat(session, wiredtiger.stat.conn.layered_curs_reopen_stable)

    def put(self, session, key, value, ts):
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor[key] = value
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def start_follower(self):
        """Both keys reach the leader's stable table; only one is replicated to the follower."""
        self.session.create(self.uri, self.table_config)
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')
        self.put(self.session, 'ingest_key', 'val', 10)
        self.put(self.session, 'stable_key', 'val', 10)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)
        self.put(session_follow, 'ingest_key', 'val', 10)

        # The follower picks up the leader's checkpoint, so the stable cursor becomes eligible.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.opened(session_follow), 0)
        return conn_follow, session_follow

    def test_lazy_stable_open(self):
        conn_follow, session_follow = self.start_follower()
        cursor_follow = session_follow.open_cursor(self.uri)

        # A search the ingest table satisfies must not open stable.
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        cursor_follow.set_key('ingest_key')
        self.assertEqual(cursor_follow.search(), 0)
        self.assertEqual(cursor_follow.get_value(), 'val')
        session_follow.commit_transaction()
        self.assertEqual(self.opened(session_follow), 0,
            'the stable cursor opened for a search the ingest table satisfied')

        # A search that misses ingest opens stable and still finds the key.
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        cursor_follow.set_key('stable_key')
        self.assertEqual(cursor_follow.search(), 0)
        self.assertEqual(cursor_follow.get_value(), 'val')
        session_follow.commit_transaction()
        self.assertEqual(self.opened(session_follow), 1)

        # A key in neither constituent misses without a second open.
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        cursor_follow.set_key('no_such_key')
        self.assertEqual(cursor_follow.search(), wiredtiger.WT_NOTFOUND)
        session_follow.commit_transaction()
        self.assertEqual(self.opened(session_follow), 1)

        cursor_follow.close()

        # search_near merges the constituents, so it opens stable even on an ingest hit.
        cursor_near = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        cursor_near.set_key('ingest_key')
        self.assertEqual(cursor_near.search_near(), 0)
        session_follow.commit_transaction()
        self.assertEqual(self.opened(session_follow), 2,
            'search_near did not open the stable cursor')

        cursor_near.close()
        conn_follow.close()

    def test_no_reopen_without_new_checkpoint(self):
        conn_follow, session_follow = self.start_follower()
        cursor_follow = session_follow.open_cursor(self.uri)

        # Two operations in a single transaction reading at a timestamp. Both miss ingest, so the
        # first opens stable; the second must not reopen it, since no newer checkpoint arrived.
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(10)}')
        cursor_follow.set_key('stable_key')
        self.assertEqual(cursor_follow.search(), 0)
        cursor_follow.set_key('stable_key')
        self.assertEqual(cursor_follow.search(), 0)
        session_follow.commit_transaction()

        self.assertEqual(self.opened(session_follow), 1)
        self.assertEqual(self.reopened(session_follow), 0,
            'the stable cursor was reopened without a newer checkpoint')

        cursor_follow.close()
        conn_follow.close()
