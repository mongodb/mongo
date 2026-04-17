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

# test_layered92.py
#   Test the reserve() operation on layered cursors for keys in different states:
#   present in stable, present in ingest, present in both, or not present.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered92(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'
    uri = 'layered:test_layered92'
    disagg_storages = gen_disagg_storages('test_layered92', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)
    conn_follow = None
    session_follow = None

    def write(self, session, key, ts):
        session.begin_transaction()
        c = session.open_cursor(self.uri)
        c[key] = 'v'
        c.close()
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

    def checkpoint(self, ts):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

    def open_follower(self):
        self.conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')

    def do_reserve(self, session, key):
        c = session.open_cursor(self.uri)
        session.begin_transaction()
        c.set_key(key)
        try:
            return c.reserve()
        finally:
            session.rollback_transaction()
            c.close()

    # Leader: writes always go to stable.
    def test_leader_key_exists(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Check that we can do reserve for a key before the checkpoint
        self.write(self.session, key=1, ts=1)
        self.assertEqual(self.do_reserve(self.session, key=1), 0)

        # And after the checkpoint
        self.checkpoint(ts=1)
        self.assertEqual(self.do_reserve(self.session, key=1), 0)

    def test_leader_key_missing(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.checkpoint(ts=1)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.do_reserve(self.session, key=99))

    def test_follower_key_in_stable_only(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.write(self.session, key=1, ts=1)
        self.checkpoint(ts=1)
        self.open_follower()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.assertEqual(self.do_reserve(self.session_follow, key=1), 0)

    def test_follower_key_in_ingest_only(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.checkpoint(ts=1)
        self.open_follower()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.write(self.session_follow, key=1, ts=2)
        self.assertEqual(self.do_reserve(self.session_follow, key=1), 0)

    def test_follower_key_in_both(self):
        # Insert the key on the leader
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.write(self.session, key=1, ts=1)
        self.checkpoint(ts=1)

        # And then on the follower
        self.open_follower()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.write(self.session_follow, key=1, ts=2)
        self.assertEqual(self.do_reserve(self.session_follow, key=1), 0)

    def test_follower_key_missing(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.checkpoint(ts=1)
        self.open_follower()
        self.disagg_advance_checkpoint(self.conn_follow)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.do_reserve(self.session_follow, key=99))
