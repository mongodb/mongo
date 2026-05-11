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
from wtscenario import make_scenarios

# test_layered_fast_truncate09.py
#   Follower truncate-list visibility coverage.
@disagg_test_class
class test_layered_fast_truncate09(wttest.WiredTigerTestCase):

    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate09')),
        ('table', dict(uri='table:test_layered_fast_truncate09')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_fast_truncate09', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)

    nitems = 1000

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest(
                "fast truncate support is not enabled"
                " - check if WT_DISAGG_FAST_TRUNCATE_BUILD is enabled in the build configuration")
        super().setUp()

        self.setup_follower()

    def session_create_config(self):
        cfg = 'key_format=i,value_format=S'
        if self.uri.startswith('table:'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    def setup_follower(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        with self.transaction(session=self.session, commit_timestamp=10):
            for key in range(1, self.nitems + 1):
                cursor[key] = 'value'
        cursor.close()

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        follower_config = (
            'disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")')
        self.reopen_conn(config=follower_config)

    def truncate_range(self, session, start, stop):
        c_start = session.open_cursor(self.uri)
        c_start.set_key(start)
        c_stop = session.open_cursor(self.uri)
        c_stop.set_key(stop)
        session.truncate(None, c_start, c_stop, None)
        c_start.close()
        c_stop.close()

    def search_key(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        ret = cursor.search()
        value = cursor.get_value() if ret == 0 else None
        cursor.close()
        return ret, value

    def search_near_key(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        exact = cursor.search_near()
        landed = cursor.get_key()
        cursor.close()
        return exact, landed

    def next_key_after(self, session, key):
        cursor = session.open_cursor(self.uri)
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.next(), 0)
        landed = cursor.get_key()
        cursor.close()
        return landed

    def commit_truncate(self, session, start, stop, commit_ts):
        with self.transaction(session=session, commit_timestamp=commit_ts):
            self.truncate_range(session, start, stop)

    def test_same_transaction_reads_own_uncommitted_truncate(self):
        with self.transaction(session=self.session, rollback=True):
            self.truncate_range(self.session, 100, 700)

            ret = self.search_key(self.session, 150)[0]
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            exact, landed = self.search_near_key(self.session, 150)
            self.assertNotEqual(exact, 0)
            if exact < 0:
                self.assertEqual(landed, 99)
                self.assertEqual(self.next_key_after(self.session, 98), 99)
            else:
                self.assertEqual(landed, 701)
                self.assertEqual(self.next_key_after(self.session, 99), 701)

    def test_other_transaction_ignores_uncommitted_truncate(self):
        with self.transaction(session=self.session, rollback=True):
            self.truncate_range(self.session, 100, 700)

            session2 = self.conn.open_session()
            try:
                with self.transaction(session=session2, rollback=True):
                    self.assertEqual(self.search_key(session2, 150), (0, 'value'))
                    self.assertEqual(self.search_near_key(session2, 150), (0, 150))
                    self.assertEqual(self.next_key_after(session2, 149), 150)
            finally:
                session2.close()

    def test_rollback_restores_visibility(self):
        with self.transaction(session=self.session, rollback=True):
            self.truncate_range(self.session, 100, 700)
            ret = self.search_key(self.session, 150)[0]
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        session2 = self.conn.open_session()
        try:
            with self.transaction(session=session2, rollback=True):
                self.assertEqual(self.search_key(session2, 150), (0, 'value'))
                self.assertEqual(self.search_near_key(session2, 150), (0, 150))
                self.assertEqual(self.next_key_after(session2, 149), 150)
        finally:
            session2.close()

    def test_committed_truncate_respects_read_timestamp(self):
        self.commit_truncate(self.session, 100, 700, 30)

        session2 = self.conn.open_session()
        try:
            with self.transaction(session=session2, read_timestamp=20, rollback=True):
                self.assertEqual(self.search_key(session2, 150), (0, 'value'))
                self.assertEqual(self.search_near_key(session2, 150), (0, 150))
                self.assertEqual(self.next_key_after(session2, 149), 150)

            with self.transaction(session=session2, read_timestamp=30, rollback=True):
                ret = self.search_key(session2, 150)[0]
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                exact, landed = self.search_near_key(session2, 150)
                self.assertNotEqual(exact, 0)
                if exact < 0:
                    self.assertEqual(landed, 99)
                    self.assertEqual(self.next_key_after(session2, 98), 99)
                else:
                    self.assertEqual(landed, 701)
                    self.assertEqual(self.next_key_after(session2, 99), 701)
        finally:
            session2.close()

    def test_overlapping_truncates_respect_timestamp_visibility(self):
        self.commit_truncate(self.session, 100, 400, 20)
        self.commit_truncate(self.session, 300, 700, 40)

        session2 = self.conn.open_session()
        try:
            with self.transaction(session=session2, read_timestamp=30, rollback=True):
                ret = self.search_key(session2, 350)[0]
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                self.assertEqual(self.search_key(session2, 500), (0, 'value'))

            with self.transaction(session=session2, read_timestamp=40, rollback=True):
                ret = self.search_key(session2, 350)[0]
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                ret = self.search_key(session2, 500)[0]
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
                exact, landed = self.search_near_key(session2, 150)
                self.assertNotEqual(exact, 0)
                if exact < 0:
                    self.assertEqual(landed, 99)
                    self.assertEqual(self.next_key_after(session2, 98), 99)
                else:
                    self.assertEqual(landed, 701)
                    self.assertEqual(self.next_key_after(session2, 99), 701)
        finally:
            session2.close()
