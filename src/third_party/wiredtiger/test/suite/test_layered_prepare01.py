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

# Layered iteration returns readable keys before prepared removes and resumes after commit or rollback.

@disagg_test_class
class test_layered_prepare01(wttest.WiredTigerTestCase):

    # stable_keys: inserted by the leader and checkpointed into stable.
    # prepared_keys: subset for which the follower prepares removes in ingest.
    # Commit keeps the removed subset hidden; rollback restores all stable keys.
    test_name = __qualname__
    scenarios = make_scenarios([
        ('only_key',  dict(stable_keys=['1'],         prepared_keys=['1'])),
        ('first',     dict(stable_keys=['1','2','3'], prepared_keys=['1'])),
        ('middle',    dict(stable_keys=['1','2','3'], prepared_keys=['2'])),
        ('last',      dict(stable_keys=['1','2','3'], prepared_keys=['3'])),
        ('first_two', dict(stable_keys=['1','2','3'], prepared_keys=['1','2'])),
        ('last_two',  dict(stable_keys=['1','2','3'], prepared_keys=['2','3'])),
        ('outer_two', dict(stable_keys=['1','2','3'], prepared_keys=['1','3'])),
    ], [
        ('rollback', dict(commit_prepared=False)),
        ('commit', dict(commit_prepared=True)),
    ])

    conn_base_config = ('precise_checkpoint=true,')
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    def safe_next(self, cursor):
        '''Call cursor.next(), returning WT_PREPARE_CONFLICT instead of raising.'''
        try:
            return cursor.next()
        except wiredtiger.WiredTigerError as e:
            if 'WT_PREPARE_CONFLICT' in str(e):
                return wiredtiger.WT_PREPARE_CONFLICT
            raise

    def test_iterate_after_prepare_resolution(self):
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() +
                ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        uri = f'table:{self.test_name}'
        self.session.create(uri, 'key_format=S,value_format=S,block_manager=disagg,type=layered')

        # Leader: insert stable_keys and checkpoint.
        with self.transaction(session=self.session, commit_timestamp=100):
            c = self.session.open_cursor(uri)
            for k in self.stable_keys:
                c[k] = k
            c.close()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(200)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Follower: prepare removes of prepared_keys into ingest.
        prep_session = conn_follow.open_session('')
        prep_cursor = prep_session.open_cursor(uri, None, 'overwrite=false')
        prep_session.begin_transaction()
        for k in self.prepared_keys:
            prep_cursor.set_key(k)
            prep_cursor.remove()
        prep_cursor.close()
        prep_session.prepare_transaction(
                f'prepare_timestamp={self.timestamp_str(300)}'
                + f',prepared_id={self.prepared_id_str(1)}')

        # Open a reader whose read_timestamp covers the prepare.
        iter_session = conn_follow.open_session('')
        iter_session.begin_transaction(f'read_timestamp={self.timestamp_str(400)}')
        cursor = iter_session.open_cursor(uri)

        expected = [(k, k) for k in self.stable_keys
            if not self.commit_prepared or k not in self.prepared_keys]
        first_prepared = min(self.prepared_keys)
        expected_prefix = [(k, k) for k in self.stable_keys if k < first_prepared]
        prefix = []
        for pair in expected_prefix:
            self.assertEqual(self.safe_next(cursor), 0)
            prefix.append((cursor.get_key(), cursor.get_value()))
            self.assertEqual(prefix[-1], pair)
        self.assertEqual(prefix, expected_prefix)

        # The first prepared key conflicts until the prepared transaction resolves.
        self.assertEqual(self.safe_next(cursor), wiredtiger.WT_PREPARE_CONFLICT)
        self.assertEqual(self.safe_next(cursor), wiredtiger.WT_PREPARE_CONFLICT)

        if self.commit_prepared:
            prep_session.commit_transaction(
                f'commit_timestamp={self.timestamp_str(350)},'
                f'durable_timestamp={self.timestamp_str(350)}')
        else:
            prep_session.rollback_transaction()

        # Continue the same cursor without resetting or repeating the prefix.
        suffix = []
        for pair in expected[len(expected_prefix):]:
            self.assertEqual(cursor.next(), 0)
            suffix.append((cursor.get_key(), cursor.get_value()))
            self.assertEqual(suffix[-1], pair)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(suffix, expected[len(expected_prefix):])
        self.assertEqual(prefix + suffix, expected)

        cursor.close()
        iter_session.rollback_transaction()
        conn_follow.close()
