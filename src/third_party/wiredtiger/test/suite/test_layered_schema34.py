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

# A table awaiting publication keeps its contents in memory and cannot be evicted. Once the
# stable schema epoch covers the table's create, eviction publishes the table itself rather
# than leaving it in memory until the next checkpoint visits the tree.

import errno, time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from wiredtiger import stat
from wtscenario import make_scenarios

# Eviction only publishes a table it walks, so the tests need it working for its cache.
conn_base_config = 'cache_size=20MB,statistics=(all),debug_mode=(eviction=true),' \
                 + 'eviction_dirty_target=1,'

@disagg_test_class
class test_layered_schema34(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__

    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 5000

    def wait_for_published(self, expected):
        """
        Wait for eviction to publish the tables under test. Eviction runs a pass when it wants
        memory, so write to a scratch table to keep the cache under pressure. The tables under
        test are left alone, so their contents stay exactly what each test wrote.
        """
        scratch = 'table:' + self.test_name + '_pressure'
        self.session.create(scratch, 'key_format=S,value_format=S')

        for i in range(600):
            published = self.get_stat(stat.conn.eviction_disagg_publish_cleared)
            if published >= expected:
                self.assertEqual(published, expected)
                return
            self.insert(scratch, i * 100, 100, 20)
            time.sleep(0.1)
        self.fail('eviction never published %d tables, saw %d' %
                  (expected, self.get_stat(stat.conn.eviction_disagg_publish_cleared)))

    def insert(self, uri, start, count, commit_ts):
        cursor = self.session.open_cursor(uri)
        for i in range(start, start + count):
            self.session.begin_transaction()
            cursor[str(i)] = 'v' * 100 + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, uri, count):
        cursor = self.session.open_cursor(uri)
        seen = 0
        while cursor.next() == 0:
            seen += 1
        cursor.close()
        self.assertEqual(seen, count)

    def test_eviction_publishes_covered_table(self):
        """Eviction publishes a table once the stable epoch covers its create."""
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.insert(uri, 0, self.nitems, 20)

        self.assertEqual(self.get_stat(stat.conn.eviction_disagg_publish_cleared), 0)

        # Published, but not yet covered.
        self.publish(uri, 10)
        self.assertEqual(self.get_stat(stat.conn.eviction_disagg_publish_cleared), 0)

        self.set_stable_epoch(10)
        self.wait_for_published(1)

        # The table is an ordinary one from here.
        self.leader_checkpoint(30)
        self.check(uri, self.nitems)

    def test_publication_lets_eviction_take_pages(self):
        """
        Publication releases the table's pages to eviction, with no checkpoint involved.
        Complements test_layered_schema26, which asserts eviction takes none of them while the
        table is still awaiting publication.
        """
        self.set_stable_epoch(1)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                ',stable_timestamp=' + self.timestamp_str(10))

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=i,value_format=S')
        self.publish(uri, 20)

        nrows = 300
        with self.transaction(commit_timestamp=30):
            with wttest.open_cursor(self.session, uri) as cursor:
                for i in range(1, nrows + 1):
                    cursor[i] = 'v' * 2048
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Uncovered, so the pages stay in memory.
        self.assertEqual(
            self.get_stat(stat.dsrc.cache_eviction_pages_seen, uri=self.stable_uri(uri)), 0)

        self.set_stable_epoch(20)
        self.assertStatGreaterSoon(
            stat.dsrc.cache_eviction_pages_seen, 0, uri=self.stable_uri(uri), timeout=60)
        self.assertGreater(self.get_stat(stat.conn.eviction_disagg_publish_cleared), 0)

        with wttest.open_cursor(self.session, uri) as cursor:
            self.assertEqual(sum(1 for _ in cursor), nrows)

    def test_eviction_publishes_every_covered_table(self):
        """Every table the epoch covers is published, and one published above it is not."""
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        covered = ['layered:' + self.test_name + str(i) for i in range(3)]
        for uri in covered:
            self.session.create(uri, 'key_format=S,value_format=S')
            self.insert(uri, 0, 100, 20)
            self.publish(uri, 10)

        above = 'layered:' + self.test_name + '_above'
        self.session.create(above, 'key_format=S,value_format=S')
        self.publish(above, 20)

        self.set_stable_epoch(10)
        self.wait_for_published(len(covered))

        self.set_stable_epoch(20)
        self.wait_for_published(len(covered) + 1)

    def test_drop_blocked_until_checkpoint(self):
        """A published table still holds uncheckpointed data, so the drop keeps being refused."""
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.insert(uri, 0, self.nitems, 20)
        self.publish(uri, 10)
        self.set_stable_epoch(10)
        self.wait_for_published(1)

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(uri, None))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertEqual(sub, wiredtiger.WT_DIRTY_DATA)
        self.assertTrue('unpublished data' in msg)

        self.leader_checkpoint(30)
        self.dropUntilSuccess(self.session, uri)

    def test_verify_after_publish(self):
        """
        Verify skips a table awaiting publication. A published one is verified like any other:
        busy while it holds dirty data, and verified once a checkpoint has run. The retry covers
        the ingest constituent, which stays dirty until its contents drain into the stable one.
        """
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.insert(uri, 0, self.nitems, 20)
        self.publish(uri, 10)
        self.set_stable_epoch(10)
        self.wait_for_published(1)

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.verify(uri, None))
        err, sub, msg = self.session.get_last_error()
        self.assertEqual(err, errno.EBUSY)
        self.assertEqual(sub, wiredtiger.WT_DIRTY_DATA)

        self.leader_checkpoint(30)
        self.verifyUntilSuccess(self.session, uri, config=None)
        self.check(uri, self.nitems)

    def test_table_above_the_epoch_keeps_waiting(self):
        """A table published above the stable epoch stays unpublished across checkpoints."""
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.publish(uri, 20)

        # Such a table may only hold data the checkpoint does not consider stable.
        self.insert(uri, 0, 100, 50)

        self.leader_checkpoint(30)
        self.assertEqual(self.get_stat(stat.conn.eviction_disagg_publish_cleared), 0)

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.drop(uri, None))
        self.check(uri, 100)

    def test_step_up_publishes(self):
        """
        A table created on a follower gets its stable constituent at step up, and the queue
        entry is the only record of the epoch it was published at.
        """
        # Reopening in disaggregated mode reports that it removed the local history store.
        self.ignoreStdoutPattern('wiredtiger_open:.*WT_VERB_METADATA')
        self.reopen_conn(config=self.conn_config_follower)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.publish(uri, 10)

        self.step_up()
        self.insert(uri, 0, self.nitems, 20)
        self.set_stable_epoch(10)
        self.wait_for_published(1)

        self.leader_checkpoint(30)
        self.check(uri, self.nitems)

    def test_follower_does_not_publish(self):
        """Only a leader writes pages, so a follower publishes nothing when the epoch advances."""
        # Reopening in disaggregated mode reports that it removed the local history store.
        self.ignoreStdoutPattern('wiredtiger_open:.*WT_VERB_METADATA')
        self.reopen_conn(config=self.conn_config_follower)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.set_stable_epoch(5)

        uri = 'layered:' + self.test_name
        self.session.create(uri, 'key_format=S,value_format=S')
        self.publish(uri, 10)

        self.set_stable_epoch(10)
        self.session.checkpoint()
        self.assertEqual(self.get_stat(stat.conn.eviction_disagg_publish_cleared), 0)
