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

# Forward iteration on a layered cursor after the very first next() returns
# WT_PREPARE_CONFLICT must resume correctly and return all visible keys.

@disagg_test_class
class test_layered_prepare03(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_base_config = 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    def safe_next(self, cursor):
        try:
            return cursor.next()
        except wiredtiger.WiredTigerError as e:
            if 'WT_PREPARE_CONFLICT' in str(e):
                return wiredtiger.WT_PREPARE_CONFLICT
            raise

    def test_iterate_after_prepare_conflict_on_first_key(self):
        '''
        A layered cursor that encounters WT_PREPARE_CONFLICT on its very first
        next() call must resume from the beginning after the conflict is resolved
        and return all stable keys.
        '''
        uri = f'table:{self.test_name}'
        stable_keys = ['1', '2', '3']

        # Write stable keys on the leader and checkpoint.
        self.session.create(
            uri, 'key_format=S,value_format=S,block_manager=disagg,type=layered')
        with self.transaction(session=self.session, commit_timestamp=100):
            c = self.session.open_cursor(uri)
            for k in stable_keys:
                c[k] = 'stable_' + k
            c.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(200)}')
        self.session.checkpoint()

        # Open a follower and pull in the stable checkpoint.
        conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config +
            'disaggregated=(role="follower")')
        self.disagg_advance_checkpoint(conn_follow)

        # Prepare an ingest update for key '1' so that the first next() on the
        # layered cursor returns WT_PREPARE_CONFLICT.
        prep_session = conn_follow.open_session('')
        prep_cursor = prep_session.open_cursor(uri)
        prep_session.begin_transaction()
        prep_cursor['1'] = 'prepared_update'
        prep_cursor.close()
        prep_session.prepare_transaction(
            f'prepare_timestamp={self.timestamp_str(300)}'
            + f',prepared_id={self.prepared_id_str(1)}')

        # Read-committed isolation: the transaction sees the prepared update as
        # a conflict on the very first next() call.
        iter_session = conn_follow.open_session('')
        iter_session.begin_transaction('isolation=read-committed')
        iter_cursor = iter_session.open_cursor(uri)

        # First next() must hit the prepared key and return WT_PREPARE_CONFLICT.
        self.assertEqual(self.safe_next(iter_cursor), wiredtiger.WT_PREPARE_CONFLICT)

        # Resolve the conflict and verify that subsequent iteration returns all
        # stable keys from the beginning.
        prep_session.rollback_transaction()

        got = []
        ret = iter_cursor.next()
        while ret == 0:
            got.append(iter_cursor.get_key())
            ret = iter_cursor.next()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(got, stable_keys)

        iter_cursor.close()
        iter_session.rollback_transaction()
        prep_session.close()
        conn_follow.close()
