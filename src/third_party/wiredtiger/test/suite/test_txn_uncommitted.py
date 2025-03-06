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

import os
from unittest import skip
import wiredtiger, wttest

# test_txn_uncommitted.py
#    Stats for uncommitted txn data.
class test_txn_uncommitted(wttest.WiredTigerTestCase):

    n_sessions = 20
    n_updates = 20
    conn_config = 'statistics=(all)'
    entry_value = "abcde" * 400

    def get_sstat(self, stat, session):
        statc =  session.open_cursor('statistics:session', None, None)
        val = statc[stat][2]
        statc.close()
        return val

    def get_cstat(self, stat):
        statc =  self.session.open_cursor('statistics:', None, None)
        val = statc[stat][2]
        statc.close()
        return val

    def txn_one(self):
        session = self.conn.open_session()
        cursor = session.open_cursor('table:test_txn_uncommitted', None, None)

        session.begin_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), 0)

        cursor[1] = self.entry_value

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 1)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), len(self.entry_value))
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 1)
        self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), len(self.entry_value))

        session.commit_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), 0)

        cursor.reset()
        session.close()

    def txn_two(self):
        session1 = self.conn.open_session()
        session2 = self.conn.open_session()
        cursor1 = session1.open_cursor('table:test_txn_uncommitted', None, None)
        cursor2 = session2.open_cursor('table:test_txn_uncommitted', None, None)

        session1.begin_transaction()
        session2.begin_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session2), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session2), 0)

        cursor1[1] = self.entry_value

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 1)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), len(self.entry_value))
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 1)
        self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), len(self.entry_value))

        cursor2[2] = self.entry_value

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 2)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 2*len(self.entry_value))
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 1)
        self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), len(self.entry_value))
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session2), 1)
        self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session2), len(self.entry_value))

        session1.commit_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 1)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), len(self.entry_value))

        session2.rollback_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session2), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session2), 0)

        session1.close()
        session2.close()

    def txn_two_seq(self):
        session1 = self.conn.open_session()
        session2 = self.conn.open_session()
        cursor1 = session1.open_cursor('table:test_txn_uncommitted', None, None)
        cursor2 = session2.open_cursor('table:test_txn_uncommitted', None, None)

        session1.begin_transaction()
        session2.begin_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session2), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session2), 0)

        cursor1[1] = self.entry_value

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 1)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), len(self.entry_value))

        session1.rollback_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)

        cursor2[2] = self.entry_value

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 1)
        self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), len(self.entry_value))

        session2.commit_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session1), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session2), 0)
        self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session2), 0)

        session1.close()
        session2.close()

    def txn_many(self):
        sessions = [self.conn.open_session() for _ in range(self.n_sessions)]
        cursors = [session.open_cursor('table:test_txn_uncommitted', None, None) for session in sessions]
        for session in sessions:
            session.begin_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)

        for i, cursor in enumerate(cursors):
            cursor[i] = self.entry_value
            self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), i+1)
            self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), (i+1)*len(self.entry_value))

        for i, session in enumerate(sessions):
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 1)
            self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), len(self.entry_value))
            session.commit_transaction()
            self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), self.n_sessions-i-1)
            self.assertGreaterEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), (self.n_sessions-i-1)*len(self.entry_value))
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 0)
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), 0)

        for session in sessions:
            session.close()

    def txn_many_many(self):
        sessions = [self.conn.open_session() for _ in range(self.n_sessions)]
        cursors = [session.open_cursor('table:test_txn_uncommitted', None, None) for session in sessions]
        for session in sessions:
            session.begin_transaction()

        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), 0)
        self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), 0)

        for i, cursor in enumerate(cursors):
            for j in range(self.n_updates):
                cursor[i*self.n_updates + j] = self.entry_value
            self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), self.n_updates * (i+1))
            self.assertGreater(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), (i+1)*len(self.entry_value) * self.n_updates)

        for i, session in enumerate(sessions):
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), self.n_updates)
            self.assertGreater(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), self.n_updates * len(self.entry_value))
            session.commit_transaction()
            self.assertEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_count), (self.n_sessions-i-1) * self.n_updates)
            self.assertGreaterEqual(self.get_cstat(wiredtiger.stat.conn.cache_updates_txn_uncommitted_bytes), (self.n_sessions-i-1)*len(self.entry_value) * self.n_updates)
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_updates, session), 0)
            self.assertEqual(self.get_sstat(wiredtiger.stat.session.txn_bytes_dirty, session), 0)

        for session in sessions:
            session.close()

    def test_session_stats(self):
        self.session = self.conn.open_session()
        self.session.create("table:test_txn_uncommitted", "key_format=i,value_format=S")

        self.txn_one()
        self.txn_two()
        self.txn_two_seq()
        self.txn_many()
        self.txn_many_many()
