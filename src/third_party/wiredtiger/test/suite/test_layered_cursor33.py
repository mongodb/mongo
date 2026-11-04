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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# On a follower, a blind remove always succeeds and never opens the stable table, wherever the key
# is and with or without a read timestamp.
@disagg_test_class
class test_layered_cursor33(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = 'layered:' + test_name
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    # Where the removed key lives on the follower, and whether the remove finds it already removed.
    targets = [
        ('ingest_live', dict(key='ingest_key', setup='write', already_removed=False)),
        ('ingest_removed', dict(key='ingest_key', setup='write_remove', already_removed=True)),
        ('truncated', dict(key='stable_key', setup='truncate', already_removed=True)),
        ('stable_only', dict(key='stable_key', setup=None, already_removed=False)),
        ('absent', dict(key='absent_key', setup=None, already_removed=False)),
    ]
    read_timestamps = [
        ('no_read_ts', dict(read_ts=None)),
        ('read_ts', dict(read_ts=25)),
    ]
    # Whether the cursor already reads stable, from a checkpoint older than the follower's newest.
    stable_states = [
        ('fresh', dict(stable_open=False)),
        ('stable_open', dict(stable_open=True)),
    ]
    scenarios = make_scenarios(disagg_storages, targets, read_timestamps, stable_states)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, self.table_config)
        self.leader_write_and_checkpoint({'anchor': 'anchor', 'stable_key': 'stable_val'}, 10)

        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, self.table_config)
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

    def leader_write_and_checkpoint(self, items, ts):
        cursor = self.session.open_cursor(self.uri)
        with self.transaction(commit_timestamp=ts):
            for key, value in items.items():
                cursor[key] = value
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

    def follower_setup_key(self):
        cursor = self.session_follow.open_cursor(self.uri)
        if self.setup in ('write', 'write_remove'):
            with self.transaction(session=self.session_follow, commit_timestamp=20):
                cursor[self.key] = 'ingest_val'
        if self.setup == 'write_remove':
            with self.transaction(session=self.session_follow, commit_timestamp=21):
                cursor.set_key(self.key)
                self.assertEqual(cursor.remove(), 0)
        if self.setup == 'truncate':
            stop = self.session_follow.open_cursor(self.uri)
            cursor.set_key(self.key)
            stop.set_key(self.key)
            with self.transaction(session=self.session_follow, commit_timestamp=20):
                self.session_follow.truncate(None, cursor, stop, None)
            stop.close()
        cursor.close()

    def follower_stats(self):
        return self.get_stats([stat.conn.layered_curs_open_stable,
            stat.conn.layered_curs_reopen_stable, stat.conn.layered_curs_blind_remove_follower,
            stat.conn.layered_curs_blind_remove_ingest_deleted], '', self.session_follow)

    def test_blind_remove(self):
        self.follower_setup_key()
        cursor = self.session_follow.open_cursor(self.uri, None, 'blind_remove=true')

        if self.stable_open:
            cursor.set_key('anchor')
            self.assertEqual(cursor.search(), 0)
            cursor.reset()
            self.leader_write_and_checkpoint({'anchor': 'anchor_15'}, 15)
            self.disagg_advance_checkpoint_and_wait(self.conn_follow)

        before = self.follower_stats()
        begin_config = None
        if self.read_ts is not None:
            begin_config = 'read_timestamp=' + self.timestamp_str(self.read_ts)
        self.session_follow.begin_transaction(begin_config)
        cursor.set_key(self.key)
        self.assertEqual(cursor.remove(), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        after = self.follower_stats()
        cursor.close()

        delta = {s: after[s] - before[s] for s in before}
        self.assertEqual(delta[stat.conn.layered_curs_open_stable], 0)
        self.assertEqual(delta[stat.conn.layered_curs_reopen_stable], 1 if self.stable_open else 0)
        self.assertEqual(delta[stat.conn.layered_curs_blind_remove_follower], 1)
        self.assertEqual(delta[stat.conn.layered_curs_blind_remove_ingest_deleted],
            1 if self.already_removed else 0)

        # The key is gone and the rest of the table is untouched.
        verify = self.session_follow.open_cursor(self.uri)
        verify.set_key(self.key)
        self.assertEqual(verify.search(), wiredtiger.WT_NOTFOUND)
        verify.set_key('anchor')
        self.assertEqual(verify.search(), 0)
        verify.close()
