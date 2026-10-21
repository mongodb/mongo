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

from contextlib import closing

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin, range_inclusive
from wtscenario import make_scenarios

# Verify that follower writes obey timestamp requirements. The truncated range holds no key
# the follower itself wrote, so the truncate-list entry is the only record of that operation.

@disagg_test_class
class test_layered_txn04(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader"),'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)

    scenarios = make_scenarios(disagg_storages)

    nitems = 100

    def assert_commit_rejected_at_checkpoint(self, session, commit_ts):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session.commit_transaction(
              'commit_timestamp=' + self.timestamp_str(commit_ts)),
            '/commit timestamp .* must be after the last checkpoint timestamp .*/')

    def test_untimestamped_follower_truncate_is_refused(self):
        keys = range_inclusive(1, self.nitems)
        self.setup_leader(keys=keys)
        self.setup_follower()

        start = self.session.open_cursor(self.uri)
        start.set_key(self.key(1))
        stop = self.session.open_cursor(self.uri)
        stop.set_key(self.key(self.nitems))
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)
        start.close()
        stop.close()

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(),
            '/commit timestamp is required for writes to disaggregated tables/')

        # The refused commit rolled the truncate back, so every key is still visible.
        self.assertEqual(self.visible_keys(), list(keys))

    def test_follower_writes_must_follow_adopted_checkpoint(self):
        keys = range_inclusive(1, self.nitems)
        self.setup_leader(keys=keys)
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        with closing(conn_follow), closing(session_follow):
            self.disagg_wait_for_adoption(conn_follow)

            self.leader_checkpoint(30)
            self.disagg_advance_checkpoint_and_wait(conn_follow)

            for commit_ts in (20, 30):
                insert = session_follow.open_cursor(self.uri)
                insert.set_key(self.key(self.nitems + 1))
                insert.set_value('inserted')
                session_follow.begin_transaction()
                insert.insert()
                insert.close()
                self.assert_commit_rejected_at_checkpoint(session_follow, commit_ts)

                update = session_follow.open_cursor(self.uri)
                update.set_key(self.key(1))
                update.set_value('updated')
                session_follow.begin_transaction()
                update.update()
                update.close()
                self.assert_commit_rejected_at_checkpoint(session_follow, commit_ts)

                remove = session_follow.open_cursor(self.uri)
                remove.set_key(self.key(2))
                session_follow.begin_transaction()
                remove.remove()
                remove.close()
                self.assert_commit_rejected_at_checkpoint(session_follow, commit_ts)

                start = session_follow.open_cursor(self.uri)
                start.set_key(self.key(1))
                stop = session_follow.open_cursor(self.uri)
                stop.set_key(self.key(self.nitems))
                session_follow.begin_transaction()
                session_follow.truncate(None, start, stop, None)
                start.close()
                stop.close()
                self.assert_commit_rejected_at_checkpoint(session_follow, commit_ts)

    def test_follower_prepares_must_follow_adopted_checkpoint(self):
        self.setup_leader(keys=range_inclusive(1, self.nitems))
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        with closing(conn_follow), closing(session_follow):
            self.disagg_wait_for_adoption(conn_follow)
            self.leader_checkpoint(30)
            self.disagg_advance_checkpoint_and_wait(conn_follow)

            with closing(session_follow.open_cursor(self.uri)) as cursor:
                for api, write_first in (('prepare', True), ('timestamp', True),
                  ('timestamp', False), ('timestamp_uint', True), ('timestamp_uint', False)):
                    for prepare_ts in (20, 30, 31):
                        session_follow.begin_transaction()
                        if write_first:
                            cursor[self.key(self.nitems + 1)] = 'prepared'
                        config = 'prepare_timestamp=' + self.timestamp_str(prepare_ts)
                        if api == 'prepare':
                            set_timestamp = lambda: session_follow.prepare_transaction(config)
                        elif api == 'timestamp':
                            set_timestamp = lambda: session_follow.timestamp_transaction(config)
                        else:
                            set_timestamp = lambda: session_follow.timestamp_transaction_uint(
                                wiredtiger.WT_TS_TXN_TYPE_PREPARE, prepare_ts)
                        if prepare_ts <= 30:
                            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                set_timestamp,
                                '/prepare timestamp .* must be after the last checkpoint timestamp .*/')
                        else:
                            set_timestamp()
                            if api != 'prepare':
                                if not write_first:
                                    cursor[self.key(self.nitems + 1)] = 'prepared'
                                session_follow.prepare_transaction()
                        session_follow.rollback_transaction()

    def test_follower_prepared_durable_must_follow_adopted_checkpoint(self):
        self.setup_leader(keys=range_inclusive(1, self.nitems))
        self.leader_checkpoint(10)

        conn_follow, session_follow = self.open_follower()
        with closing(conn_follow), closing(session_follow):
            self.disagg_wait_for_adoption(conn_follow)

            # Each case resolves its own prepared transaction, since a rejected timestamp leaves
            # a prepared transaction that can only be rolled back.
            durable_timestamps = (40, 50, 51)
            sessions = [conn_follow.open_session() for _ in durable_timestamps]
            cursors = [session.open_cursor(self.uri) for session in sessions]
            for i, (session, cursor) in enumerate(zip(sessions, cursors)):
                session.begin_transaction()
                cursor[self.key(self.nitems + 1 + i)] = 'prepared'
                session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

            # The checkpoint moves past the prepared transactions before they resolve. A commit
            # timestamp may stay behind the checkpoint, but a durable timestamp may not.
            self.leader_checkpoint(50)
            self.disagg_advance_checkpoint_and_wait(conn_follow)

            for session, durable_ts in zip(sessions, durable_timestamps):
                config = ('commit_timestamp=' + self.timestamp_str(40) +
                          ',durable_timestamp=' + self.timestamp_str(durable_ts))
                if durable_ts <= 50:
                    self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                        lambda: session.timestamp_transaction(config),
                        '/durable timestamp .* must be after the last checkpoint timestamp .*/')
                    session.rollback_transaction()
                else:
                    session.timestamp_transaction(config)
                    session.commit_transaction()

            for cursor in cursors:
                cursor.close()
            for session in sessions:
                session.close()
