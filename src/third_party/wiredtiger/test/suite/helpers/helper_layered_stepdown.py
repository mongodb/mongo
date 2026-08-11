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

# helper_layered_stepdown.py
#   Shared helpers for the async (planned) step-down layered table tests.

import wiredtiger
from wiredtiger import stat

# Shared helpers for the layered async step-down test suite.
class LayeredStepdownMixin:
    # Set the global oldest and stable timestamps.
    def set_global_ts(self, oldest, stable):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(oldest) +
                                ',stable_timestamp=' + self.timestamp_str(stable))

    # Set the planned step-down timestamp at the given cutoff.
    def set_step_down_ts(self, ts):
        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(ts))

    # Complete a planned step-down: advance stable to the cutoff, take the step-down checkpoint
    # and demote to follower.
    def complete_step_down(self, cutoff):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(cutoff))
        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()
        self.conn.reconfigure('disaggregated=(role="follower")')

    # The file URI of a layered table's ingest constituent.
    def ingest_uri(self, uri):
        return 'file:' + uri.split(':', 1)[1] + '.wt_ingest'

    # The file URI of a layered table's stable constituent.
    def stable_uri(self, uri):
        return 'file:' + uri.split(':', 1)[1] + '.wt_stable'

    # Every layered table URI in the connection's local metadata.
    def layered_tables(self, conn=None):
        session = (conn or self.conn).open_session('')
        cursor = session.open_cursor('metadata:')
        uris = [k for k, _ in cursor if k.startswith('layered:')]
        cursor.close()
        session.close()
        return uris

    # The local metadata configuration of a layered table's stable constituent, or None when the
    # constituent has no row. Read the metadata directly: opening a cursor on the constituent
    # reports any error as absence, which cannot be told apart from a transactional failure.
    def stable_metadata(self, conn, uri):
        session = (conn or self.conn).open_session('')
        cursor = session.open_cursor('metadata:')
        cursor.set_key(self.stable_uri(uri))
        config = cursor.get_value() if cursor.search() == 0 else None
        cursor.close()
        session.close()
        return config

    # Whether a layered table's stable constituent has a row in the local metadata.
    def stable_constituent_exists(self, conn, uri):
        return self.stable_metadata(conn, uri) is not None

    # Whether a layered table's stable constituent has been checkpointed. A constituent can exist
    # without a checkpoint: a checkpoint withholds the pages of a table awaiting publication.
    def stable_is_checkpointed(self, conn, uri):
        config = self.stable_metadata(conn, uri)
        return config is not None and 'checkpoint=(' in config

    # Assert all three states of a layered table at once. They are maintained by different code and
    # can legally disagree, so a test that checks one of them proves little.
    def assert_table_state(self, conn, uri, constituent, checkpointed, shared):
        actual = (self.stable_constituent_exists(conn, uri),
                  self.stable_is_checkpointed(conn, uri),
                  self.uri_in_shared_metadata(conn, uri))
        self.assertEqual(actual, (constituent, checkpointed, shared),
            f'{uri}: expected (constituent, checkpointed, shared) '
            f'{(constituent, checkpointed, shared)}, got {actual}')

    # Assert the local metadata holds exactly the expected layered tables. Per-table assertions only
    # cover the tables a test names, so enumerate to catch one drifting into the wrong state.
    def assert_no_unexpected_tables(self, conn, expected):
        self.assertEqual(sorted(self.layered_tables(conn)), sorted(expected))

    # The connection's all_durable timestamp as an integer.
    def all_durable(self):
        return int(self.conn.query_timestamp('get=all_durable'), 16)

    # Write k/v pairs (dict) to a table in one transaction committed at commit_ts.
    def write_at(self, uri, items, commit_ts):
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for k, v in items.items():
            cursor[k] = v
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    # Remove keys (iterable) from a table in one transaction committed at commit_ts.
    def remove_at(self, uri, keys, commit_ts):
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        for k in keys:
            cursor.set_key(k)
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    # The key/value map visible through a cursor on uri at read_ts.
    def read_kvs_at(self, uri, read_ts):
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        kv = {}
        while cursor.next() == 0:
            kv[cursor.get_key()] = cursor.get_value()
        self.session.rollback_transaction()
        cursor.close()
        return kv

    # The set of keys visible through a cursor on uri at read_ts.
    def read_keys_at(self, uri, read_ts):
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        keys = set()
        while cursor.next() == 0:
            keys.add(cursor.get_key())
        self.session.rollback_transaction()
        cursor.close()
        return keys

    # Whether the connection currently has a step-down timestamp set. This is the only external
    # view of the timestamp, so it is also the only way to see that the demotion cleared it.
    def step_down_ts_is_set(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat.conn.txn_stepdown_ts_set][2]
        stat_cursor.close()
        return value

    # The connection-wide count of step-down transaction rollbacks.
    def get_step_down_rollback_count(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        count = stat_cursor[stat.conn.txn_rollback_stepdown][2]
        stat_cursor.close()
        return count

    # Run op and expect WT_ROLLBACK, with no claim about the reason.
    def expect_rollback(self, op):
        self.assertRaisesException(wiredtiger.WiredTigerError, op,
            wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))

    # Run op and expect a WT_ROLLBACK that is a genuine write conflict.
    def expect_conflict_rollback(self, op):
        before = self.get_step_down_rollback_count()
        self.expect_rollback(op)
        self.assertEqual(self.get_step_down_rollback_count(), before,
            'the rollback came from the step-down guard, not from conflict detection')

    # Run op and expect a WT_ROLLBACK carrying the step-down reason.
    def assert_step_down_rollback(self, op, session=None):
        before = self.get_step_down_rollback_count()
        self.assertRaisesException(wiredtiger.WiredTigerError, op,
            wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK))
        err, _, err_msg = (session or self.session).get_last_error()
        self.assertEqual(err, wiredtiger.WT_ROLLBACK)
        self.assertTrue('straddled the step-down timestamp setting boundary' in err_msg,
            'expected a step-down rollback reason, got: ' + err_msg)
        self.assertEqual(self.get_step_down_rollback_count(), before + 1)
