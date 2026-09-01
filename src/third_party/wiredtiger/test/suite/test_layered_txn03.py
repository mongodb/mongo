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

# test_layered_txn03.py
#   Timestamp boundaries on a layered table, checked on the leader and on a follower that reached
#   the same data through a checkpoint. Reads below the oldest timestamp are refused rather than
#   served a version garbage collection may already have discarded, reads at or above it keep
#   returning the right version as the oldest timestamp advances, and a commit timestamp at or
#   below the stable timestamp is refused.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_txn03(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    # The node the reads and the refused commits are issued against.
    nodes = [
        ('leader', dict(node='leader')),
        ('follower', dict(node='follower')),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, nodes)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),' \
            + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,statistics=(all),' \
            + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

    # The connection and session the scenario reads through.
    def target(self):
        if self.node == 'leader':
            return self.conn, self.session
        return self.conn_follow, self.session_follow

    # Commit one version of the key per timestamp on the leader, then checkpoint at the newest and
    # let the follower pick the result up, so both nodes hold the same version history.
    def seed_versions(self, key, timestamps):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(self.uri)
        for ts in timestamps:
            self.session.begin_transaction()
            cursor[key] = f'v{ts}'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()
        # Both nodes track the stable timestamp, and the follower needs it to police commits too.
        for conn in (self.conn, self.conn_follow):
            conn.set_timestamp('stable_timestamp=' + self.timestamp_str(timestamps[-1]))
        self.session.checkpoint()
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

    # Advance the oldest timestamp on both nodes; each connection tracks its own.
    def set_oldest(self, ts):
        for conn in (self.conn, self.conn_follow):
            conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts))

    # The value read at a timestamp. Any extra begin_transaction configuration goes ahead of the
    # read timestamp, which is validated as it is parsed.
    def read_at(self, key, ts, config=''):
        _, session = self.target()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction(config + 'read_timestamp=' + self.timestamp_str(ts))
        cursor.set_key(key)
        value = cursor.get_value() if cursor.search() == 0 else None
        session.rollback_transaction()
        cursor.close()
        return value

    # A read timestamp below the oldest timestamp is refused, so a reader can never silently see a
    # version that garbage collection is entitled to have discarded.
    def test_read_before_oldest_is_refused(self):
        self.seed_versions('k', [10, 20, 30])
        self.set_oldest(20)

        _, session = self.target()
        session.begin_transaction()

        # The refusal is reported through the return value, so match on the exception rather than
        # on the error output.
        def refuse():
            with self.assertRaises(wiredtiger.WiredTigerError) as caught:
                session.timestamp_transaction('read_timestamp=' + self.timestamp_str(10))
            self.assertIn('Invalid argument', str(caught.exception))

        if wiredtiger.standalone_build():
            refuse()
        else:
            # This is a MongoDB message, not written in standalone builds.
            with self.expectedStdoutPattern('less than the oldest timestamp'):
                refuse()

        session.rollback_transaction()

    # Rounding the read timestamp up moves it to the oldest timestamp, giving the version visible
    # there rather than the one the discarded timestamp asked for.
    def test_read_before_oldest_rounds_up(self):
        self.seed_versions('k', [10, 20, 30])
        self.set_oldest(20)

        self.assertEqual(self.read_at('k', 10, 'roundup_timestamps=(read=true),'), 'v20')

    # Advancing the oldest timestamp leaves every version at or above it readable.
    def test_oldest_advance_preserves_visible_versions(self):
        self.seed_versions('k', [10, 20, 30])

        self.assertEqual(self.read_at('k', 10), 'v10')
        self.assertEqual(self.read_at('k', 20), 'v20')
        self.assertEqual(self.read_at('k', 30), 'v30')

        # Advance past the first version and checkpoint, so garbage collection is both permitted to
        # discard it and given the occasion to do so.
        self.set_oldest(20)
        self.session.checkpoint()
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

        self.assertEqual(self.read_at('k', 20), 'v20')
        self.assertEqual(self.read_at('k', 30), 'v30')

    # A commit timestamp at or below the stable timestamp is refused; the stable timestamp is the
    # boundary a checkpoint has already committed to, so writing under it would change the past.
    def test_commit_at_or_before_stable_is_refused(self):
        self.seed_versions('k', [10, 20, 30])

        _, session = self.target()
        for ts in (20, 30):
            cursor = session.open_cursor(self.uri)
            session.begin_transaction()
            cursor['k'] = 'too-old'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: session.commit_transaction(
                    'commit_timestamp=' + self.timestamp_str(ts)), '/Invalid argument/')
            # The refused commit already resolved the transaction.
            cursor.close()

        # The refused commits left nothing behind.
        self.assertEqual(self.read_at('k', 30), 'v30')

if __name__ == '__main__':
    wttest.run()
