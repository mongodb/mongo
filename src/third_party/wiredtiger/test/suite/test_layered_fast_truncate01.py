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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_fast_truncate01.py
#   Test basic fast truncate functionality.
@disagg_test_class
class test_layered_fast_truncate01(wttest.WiredTigerTestCase):

    conn_config = 'disaggregated=(role="leader"),'

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate01')),
        ('table', dict(uri='table:test_layered_fast_truncate01')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_fast_truncate01', disagg_only = True)

    scenarios = make_scenarios(disagg_storages, uris)

    nitems = 1000

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled.")
        super().setUp()

    def session_create_config(self):
        cfg = 'key_format=S,value_format=S'
        if self.uri.startswith('table'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    def test_truncate_basic(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        value1 = "a" * 100

        # Populate data on leader.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction()
        cursor.close()
        self.session.checkpoint()

        # Switch to follower.
        follower_config = 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)

        c1 = self.session.open_cursor(self.uri)
        c1.set_key(str(100))
        c2 = self.session.open_cursor(self.uri)
        c2.set_key(str(700))

        # Create an uncommitted truncate.
        self.session.begin_transaction()
        self.session.truncate(None, c1, c2, None)

        # The second session.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(self.uri)

        # Before commit, the keys should be found.
        session2.begin_transaction()
        cursor2.set_key(str(150))
        self.assertEqual(cursor2.search(), 0)
        session2.rollback_transaction()

        self.session.commit_transaction()

        # After commit, the keys should not found.
        session2.begin_transaction()
        cursor2.set_key(str(150))
        self.assertEqual(cursor2.search(), wiredtiger.WT_NOTFOUND)
        session2.commit_transaction()
        cursor2.close()
        self.session.checkpoint()

        session2.close()
        c1.close()
        c2.close()


    def test_truncate_rollback(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        value1 = "a" * 100

        # Populate data on leader.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction()
        cursor.close()

        self.session.checkpoint()

        # Switch to follower.
        follower_config = 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)

        c1 = self.session.open_cursor(self.uri)
        c1.set_key(str(100))
        c2 = self.session.open_cursor(self.uri)
        c2.set_key(str(700))

        self.session.begin_transaction()
        self.session.truncate(None, c1, c2, None)
        self.session.rollback_transaction()

        # All data should be visible.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor.set_key(str(150))
        self.assertEqual(cursor.search(), 0)
        self.session.commit_transaction()
        cursor.close()

        c1.close()
        c2.close()

    def test_truncate_write_conflict_1(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        value1 = "a" * 100

        # Populate data on leader.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction()
        cursor.close()

        self.session.checkpoint()

        # Switch to follower.
        follower_config = 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)

        c1 = self.session.open_cursor(self.uri)
        c1.set_key(str(100))
        c2 = self.session.open_cursor(self.uri)
        c2.set_key(str(700))

        # Create an uncommitted truncate.
        self.session.begin_transaction()
        self.session.truncate(None, c1, c2, None)

        # Insert an uncommitted value in the second session.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        cursor2 = session2.open_cursor(self.uri)
        cursor2.set_key(str(150))
        cursor2.set_value("hi")
        msg1 = '/conflict between concurrent operations/'
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor2.update(), msg1)
        cursor2.close()

        session2.commit_transaction()
        session2.close()
        c1.close()
        c2.close()

    def test_truncate_write_conflict_2(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri)
        value1 = "a" * 100

        # Populate data on leader.
        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            self.session.commit_transaction()

        self.session.checkpoint()
        cursor.close()

        # Switch to follower.
        follower_config = 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)

        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Insert an uncommitted value in the second session.
        cursor2 = session2.open_cursor(self.uri)
        cursor2.set_key(str(100))
        cursor2.set_value("hi")
        cursor2.update()
        cursor2.close()

        # Create an uncommitted truncate.
        c1 = self.session.open_cursor(self.uri)
        c1.set_key(str(100))
        c2 = self.session.open_cursor(self.uri)
        c2.set_key(str(700))

        self.session.begin_transaction()
        msg1 = '/conflict between concurrent operations/'
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: self.session.truncate(None, c1, c2, None), msg1)

        self.session.rollback_transaction()
        session2.commit_transaction()
        session2.close()
        c1.close()
        c2.close()
