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
from helper_disagg import disagg_test_class
from wtscenario import make_scenarios

# Forward iteration after a search() or search_near() that returns
# WT_PREPARE_CONFLICT must yield correct results on a layered cursor.

@disagg_test_class
class test_layered_prepare02(wttest.WiredTigerTestCase):

    test_name = __qualname__
    scenarios = make_scenarios([
        ('search',      dict(use_search_near=False)),
        ('search_near', dict(use_search_near=True)),
    ])

    conn_base_config = 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    def safe_next(self, cursor):
        try:
            return cursor.next()
        except wiredtiger.WiredTigerError as e:
            if 'WT_PREPARE_CONFLICT' in str(e):
                return wiredtiger.WT_PREPARE_CONFLICT
            raise

    def safe_search(self, cursor, key):
        cursor.set_key(key)
        try:
            cursor.search()
            return 0
        except wiredtiger.WiredTigerError as e:
            if 'WT_PREPARE_CONFLICT' in str(e):
                return wiredtiger.WT_PREPARE_CONFLICT
            raise

    def safe_search_near(self, cursor, key):
        cursor.set_key(key)
        try:
            cursor.search_near()
            return 0
        except wiredtiger.WiredTigerError as e:
            if 'WT_PREPARE_CONFLICT' in str(e):
                return wiredtiger.WT_PREPARE_CONFLICT
            raise

    def conflict_search(self, cursor, key):
        if self.use_search_near:
            return self.safe_search_near(cursor, key)
        return self.safe_search(cursor, key)

    def _setup_follower(self, prepared_key):
        '''
        Open a follower with keys '1','2','3' checkpointed as stable and one
        prepared ingest update on prepared_key.  Return (stable_keys,
        conn_follow, prep_session, iter_session, iter_cursor).
        '''
        uri = f'table:{self.test_name}'
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() +
                ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')

        stable_keys = ['1', '2', '3']
        self.session.create(uri, 'key_format=S,value_format=S,block_manager=disagg,type=layered')
        with self.transaction(session=self.session, commit_timestamp=100):
            c = self.session.open_cursor(uri)
            for k in stable_keys:
                c[k] = 'stable_' + k
            c.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(200)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        prep_session = conn_follow.open_session('')
        prep_cursor = prep_session.open_cursor(uri)
        prep_session.begin_transaction()
        prep_cursor[prepared_key] = 'prepared_update'
        prep_cursor.close()
        prep_session.prepare_transaction(
                f'prepare_timestamp={self.timestamp_str(300)}'
                + f',prepared_id={self.prepared_id_str(1)}')

        # Reader whose read_timestamp covers the prepare so it sees the conflict.
        iter_session = conn_follow.open_session('')
        iter_session.begin_transaction(f'read_timestamp={self.timestamp_str(400)}')
        iter_cursor = iter_session.open_cursor(uri)

        return stable_keys, conn_follow, prep_session, iter_session, iter_cursor

    def test_next_after_prepare_conflict(self):
        # A search that succeeds followed by a search/search_near that returns
        # WT_PREPARE_CONFLICT must leave the cursor fully reset so that subsequent
        # next() calls iterate all keys from the beginning.
        stable_keys, conn_follow, prep_session, iter_session, cursor = \
            self._setup_follower('2')

        # Position cursor at '3', then trigger WT_PREPARE_CONFLICT on '2'.
        # The cursor must reset regardless of where it was previously positioned.
        self.assertEqual(self.safe_search(cursor, '3'), 0)
        self.assertEqual(self.conflict_search(cursor, '2'), wiredtiger.WT_PREPARE_CONFLICT)

        # After rolling back the prepare, iteration must return all stable keys.
        prep_session.rollback_transaction()
        got = []
        ret = cursor.next()
        while ret == 0:
            got.append(cursor.get_key())
            ret = cursor.next()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(got, stable_keys)

        cursor.close()
        iter_session.rollback_transaction()
        conn_follow.close()

    def test_next_after_conflicting_next_then_search(self):
        # A next() that returns WT_PREPARE_CONFLICT followed by a search/search_near
        # on the same prepared key must leave the cursor fully reset so that
        # subsequent next() calls iterate all keys from the beginning.
        stable_keys, conn_follow, prep_session, iter_session, cursor = \
            self._setup_follower('1')

        # next() conflicts on the first key; search/search_near on the same key
        # also conflicts.  Both must leave the cursor in a clean state.
        self.assertEqual(self.safe_next(cursor), wiredtiger.WT_PREPARE_CONFLICT)
        self.assertEqual(self.conflict_search(cursor, '1'), wiredtiger.WT_PREPARE_CONFLICT)

        # After rolling back the prepare, iteration must return all stable keys.
        prep_session.rollback_transaction()
        got = []
        ret = cursor.next()
        while ret == 0:
            got.append(cursor.get_key())
            ret = cursor.next()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(got, stable_keys)

        cursor.close()
        iter_session.rollback_transaction()
        conn_follow.close()
