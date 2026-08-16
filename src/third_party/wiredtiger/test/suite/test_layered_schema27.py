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

# A failed drop of a layered table must be retryable: the failure leaves the table fully
# intact and the retry removes it exactly once.

import errno
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered_schema27(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'

    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S'
    nrows = 5000
    value = 'v' * 100

    disagg_storages = gen_disagg_storages(disagg_only=True)
    # With cursor caching disabled, closing the pinning cursor releases the data handle
    # immediately and the retried drop succeeds on the first attempt. With caching
    # enabled, the cached cursor keeps the handle pinned until the session's cursor
    # cache lets go, so the retry needs the standard retry loop.
    cache_scenarios = [
        ('cache_cursors', dict(cache_cursors=True)),
        ('no_cache_cursors', dict(cache_cursors=False)),
    ]
    scenarios = make_scenarios(disagg_storages, cache_scenarios)

    def conn_config(self):
        return self.conn_base_config + \
            f'cache_cursors={str(self.cache_cursors).lower()},' + \
            'disaggregated=(role="leader",lose_all_my_data=true)'

    def conn_config_follower(self):
        return self.conn_base_config + \
            f'cache_cursors={str(self.cache_cursors).lower()},' + \
            'disaggregated=(role="follower",lose_all_my_data=true)'

    # Check the table's visibility in the latest checkpoint via a fresh follower.
    def table_exists_on_follower(self, uri):
        conn_follower = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower())
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        self.disagg_advance_checkpoint(conn_follower)
        session_follower = conn_follower.open_session('')
        try:
            session_follower.open_cursor(uri).close()
            exists = True
        except wiredtiger.WiredTigerError:
            exists = False
        conn_follower.close()
        return exists

    def create_published(self, epoch):
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, epoch)

    def drop_published(self, epoch):
        if self.cache_cursors:
            self.dropUntilSuccess(self.session, self.uri)
        else:
            self.session.drop(self.uri)
        self.publish(self.uri, epoch)

    def write_rows(self, commit_ts):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = self.value
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def assert_all_rows(self):
        cursor = self.session.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            self.assertEqual(cursor.get_value(), self.value)
            count += 1
        cursor.close()
        self.assertEqual(count, self.nrows)

    # Attempt a drop that is refused because a cursor holds the table open.
    def assert_drop_busy(self):
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(self.uri))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertIn(sub, (wiredtiger.WT_NONE, wiredtiger.WT_CONFLICT_DHANDLE))

    # Make schema operations up to the given epoch durable.
    def checkpoint_epoch(self, epoch, ckpt_ts):
        self.set_stable_epoch(epoch)
        self.leader_checkpoint(ckpt_ts)

    # Check that the layered table and both constituents are in the local metadata.
    def assert_local_metadata(self, present):
        cursor = self.session.open_cursor('metadata:')
        for uri in (self.uri, f'file:{self.test_name}.wt_ingest',
                    f'file:{self.test_name}.wt_stable'):
            cursor.set_key(uri)
            self.assertEqual(cursor.search() == 0, present, uri)
        cursor.close()

    def read_stat(self, stat_key):
        cursor = self.session.open_cursor('statistics:')
        value = cursor[stat_key][2]
        cursor.close()
        return value

    def database_size(self):
        return self.read_stat(stat.conn.disagg_database_size)

    def test_failed_drop_then_retry(self):
        """
        A failed drop can be retried: the table survives the failure, and the retry
        removes it exactly once, including its share of the database size.
        """
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.checkpoint_epoch(5, 2)
        size_before_create = self.database_size()

        self.create_published(10)
        self.write_rows(commit_ts=20)
        self.checkpoint_epoch(10, 30)
        size_with_table = self.database_size()
        self.assertGreater(size_with_table, size_before_create)

        pin = self.session.open_cursor(self.uri)
        self.assert_drop_busy()

        # The table must survive the failed drop: the local metadata is rolled back and
        # the data stays intact, including in the next checkpoint.
        self.assert_local_metadata(present=True)
        self.assert_all_rows()
        self.leader_checkpoint(40)
        self.assertTrue(self.table_exists_on_follower(self.uri))

        # The failed drop must leave nothing in the shared metadata queue: a further
        # checkpoint must find no unstable operations to defer. The checkpoint updates
        # the statistic synchronously, so no waiting is needed after it returns.
        deferred_before = self.read_stat(stat.conn.checkpoint_disagg_metadata_unstable)
        self.leader_checkpoint(45)
        self.assertEqual(
            self.read_stat(stat.conn.checkpoint_disagg_metadata_unstable), deferred_before)

        # Retry the drop. A leftover REMOVE from the failed attempt would subtract the
        # table's size from the database size a second time.
        pin.close()
        self.drop_published(15)
        self.assert_local_metadata(present=False)
        self.checkpoint_epoch(15, 50)
        self.assertFalse(self.table_exists_on_follower(self.uri))
        size_after_drop = self.database_size()
        self.assertLess(size_after_drop, size_with_table)
        self.assertGreaterEqual(size_after_drop, size_before_create)

        # The name must be reusable.
        self.create_published(20)
        self.write_rows(commit_ts=60)
        self.checkpoint_epoch(20, 70)
        self.assertTrue(self.table_exists_on_follower(self.uri))
