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
#
# A prepared transaction replicated to a follower is captured as prepared by a checkpoint the
# follower adopts while its stable timestamp is behind that checkpoint. The follower rolls the
# transaction back and then steps up. The rollback timestamp must follow the adopted checkpoint,
# and the rolled-back state must hold on the new leader and in its next checkpoint.

import os, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_stepup18(wttest.WiredTigerTestCase):
    uri = 'layered:test_layered_stepup18'
    prepared_id = 1

    conn_base_config = 'statistics=(fast),precise_checkpoint=true,preserve_prepared=true,'
    disagg_storages = gen_disagg_storages(disagg_only=True)
    # The rolled-back key reads as it did before the prepare.
    ops = [
        ('insert', dict(op='insert', key=2, value=None)),
        ('update', dict(op='update', key=1, value='committed')),
        ('remove', dict(op='remove', key=1, value='committed')),
    ]
    scenarios = make_scenarios(disagg_storages, ops)

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def open_follower(self, home):
        return self.wiredtiger_open(home, self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')

    def checkpoint(self, conn):
        session = conn.open_session()
        session.checkpoint()
        session.close()

    def prepare(self, conn):
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        if self.op == 'remove':
            cursor.set_key(self.key)
            self.assertEqual(cursor.remove(), 0)
        else:
            cursor[self.key] = 'prepared'
        cursor.close()
        session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(self.prepared_id))
        return session

    def read(self, conn, ts):
        session = conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        cursor.set_key(self.key)
        try:
            ret = cursor.search()
            return cursor.get_value() if ret == 0 else None
        except wiredtiger.WiredTigerError as e:
            self.fail(f'reading key {self.key} at timestamp {ts} failed: {e}')
        finally:
            session.rollback_transaction()
            cursor.close()
            session.close()

    def check_reads(self, conn):
        for ts in (90, 200):
            self.assertEqual(self.read(conn, ts), self.value)

    def test_follower_rollback_resolved_by_step_up(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50) +
            ',stable_timestamp=' + self.timestamp_str(50))
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'committed'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))
        self.checkpoint(self.conn)

        conn_follow = self.open_follower('follower')
        session_follow = conn_follow.open_session()
        session_follow.create(self.uri, 'key_format=i,value_format=S')
        self.disagg_advance_checkpoint_and_wait(conn_follow)
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        # The same prepared transaction runs on both nodes, as if replicated.
        leader_prepared = self.prepare(self.conn)
        follower_prepared = self.prepare(conn_follow)

        # The leader checkpoints the prepared update and the follower adopts that checkpoint while
        # its own stable timestamp stays behind it.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self.checkpoint(self.conn)
        self.disagg_advance_checkpoint_and_wait(conn_follow)

        # A rollback timestamp between the follower's stable timestamp and the adopted checkpoint
        # timestamp is rejected. A rollback_transaction call whose timestamp is rejected panics
        # while prepared, so the timestamp is set first and the rollback takes none.
        try:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: follower_prepared.timestamp_transaction(
                    'rollback_timestamp=' + self.timestamp_str(120)),
                '/rollback timestamp .* must be after the last checkpoint timestamp .*/')
            follower_prepared.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(151))
        finally:
            follower_prepared.rollback_transaction()
        follower_prepared.close()

        leader_prepared.rollback_transaction('rollback_timestamp=' + self.timestamp_str(210))
        leader_prepared.close()

        # Hand leadership to the follower without a further checkpoint from the old leader.
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn_follow.reconfigure('disaggregated=(role="leader")')
        self.check_reads(conn_follow)

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self.checkpoint(conn_follow)
        self.check_reads(conn_follow)
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta(conn_follow)
        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')

        # A node opening the new leader's checkpoint finds nothing left prepared.
        os.mkdir('follower2')
        os.symlink('../kv_home', 'follower2/kv_home', target_is_directory=True)
        conn_new = self.open_follower('follower2')
        conn_new.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        session_new = conn_new.open_session()
        discover = session_new.open_cursor('prepared_discover:')
        self.assertEqual(discover.next(), wiredtiger.WT_NOTFOUND)
        discover.close()
        session_new.close()
        self.check_reads(conn_new)
        conn_new.close()
