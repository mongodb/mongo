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
class test_layered_follower18(wttest.WiredTigerTestCase):
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

    def put(self, session, key, value, ts):
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor[key] = value
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    # overwrite=true is the default, but state it: the skip-stable path this test covers is gated
    # on it. The transaction sets no read timestamp, which is the other half of that gate.
    def remove(self, session, key, ts):
        cursor = session.open_cursor(self.uri, None, 'overwrite=true')
        session.begin_transaction()
        cursor.set_key(key)
        ret = cursor.remove()
        if ret == 0:
            session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        else:
            session.rollback_transaction()
        cursor.close()
        return ret

    def search(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        ret = cursor.search()
        cursor.close()
        return ret

    def open_follower(self):
        conn = self.wiredtiger_open('follower', self.follower_config())
        session = conn.open_session('')
        session.create(self.uri, self.table_config)
        return conn, session

    def test_blind_remove_of_stable_only_key(self):
        """
        A follower's overwrite=true remove of a key that lives only in the stable constituent must
        succeed, opening stable on demand, and both nodes must then agree the key is gone.
        """

        self.session.create(self.uri, self.table_config)

        # Both nodes apply the insert. On the follower it lands in the ingest table.
        self.put(self.session, 'key_0', 'val_0', 10)
        conn_follow, session_follow = self.open_follower()
        self.put(session_follow, 'key_0', 'val_0', 10)

        # Checkpoint so the key is durable in stable, then restart the follower. Ingest does not
        # survive, so afterwards the key is only reachable through stable.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()
        session_follow.close()
        conn_follow.close()
        conn_follow, session_follow = self.open_follower()
        self.disagg_advance_checkpoint(conn_follow)

        # The leader removes the key from stable.
        self.assertEqual(self.remove(self.session, 'key_0', 20), 0)

        # The follower removes the same key. Nothing has read through this connection yet, so the
        # cursor has never opened stable; the ingest lookup misses and stable is opened on demand.
        opens_before = self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable)
        self.assertEqual(self.remove(session_follow, 'key_0', 20), 0)
        opens_after = self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable)
        self.assertGreater(opens_after, opens_before)

        # The tombstone shadows the stable value, so neither node sees the key.
        self.assertEqual(self.search(self.session, 'key_0'), wiredtiger.WT_NOTFOUND)
        self.assertEqual(self.search(session_follow, 'key_0'), wiredtiger.WT_NOTFOUND)

        session_follow.close()
        conn_follow.close()
