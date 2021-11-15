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
#
# test_timestamp04.py
#   Timestamps: Test that rollback_to_stable obeys expected visibility rules.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

class test_timestamp04(wttest.WiredTigerTestCase, suite_subprocess):
    table_ts_log     = 'table:ts04_ts_logged'
    table_ts_nolog   = 'table:ts04_ts_nologged'
    table_nots_log   = 'table:ts04_nots_logged'
    table_nots_nolog = 'table:ts04_nots_nologged'

    conncfg = [
        ('nolog', dict(conn_config=',eviction_dirty_trigger=50,eviction_updates_trigger=50',
         using_log=False)),
        ('V1', dict(conn_config=',eviction_dirty_trigger=50,eviction_updates_trigger=50,' \
         'log=(enabled),compatibility=(release="2.9")', using_log=True)),
        ('V2', dict(conn_config=',eviction_dirty_trigger=50,eviction_updates_trigger=50,' \
         'log=(enabled)', using_log=True)),
    ]
    session_config = 'isolation=snapshot'

    # Minimum cache_size requirement of lsm is 31MB.
    types = [
        # FLCS does not yet work in a timestamp world.
        ('col_fix', dict(empty=1, \
          cacheSize='cache_size=20MB', extra_config=',key_format=r,value_format=8t')),
        ('lsm', dict(empty=0, cacheSize='cache_size=31MB', extra_config=',type=lsm')),
        ('row', dict(empty=0, cacheSize='cache_size=20MB', extra_config='',)),
        ('row-smallcache', dict(empty=0, cacheSize='cache_size=2MB', extra_config='',)),
        ('var', dict(empty=0, cacheSize='cache_size=20MB', extra_config=',key_format=r')),
    ]

    scenarios = make_scenarios(conncfg, types)

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, tablename, expected, missing=False, prn=False):
        if txn_config:
            session.begin_transaction(txn_config)
        cur = session.open_cursor(tablename, None)
        if missing == False:
            actual = dict((k, v) for k, v in cur if v != 0)
            if actual != expected:
                print("missing: ", sorted(set(expected.items()) - set(actual.items())))
                print("extras: ", sorted(set(actual.items()) - set(expected.items())))
            self.assertTrue(actual == expected)

        # Search for the expected items as well as iterating.
        for k, v in expected.items():
            if missing == False:
                self.assertEqual(cur[k], v, "for key " + str(k) +
                    " expected " + str(v) + ", got " + str(cur[k]))
            else:
                cur.set_key(k)
                if self.empty:
                    # Fixed-length column-store rows always exist.
                    self.assertEqual(cur.search(), 0)
                else:
                    self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        cur.close()
        if txn_config:
            session.commit_transaction()

    # This test varies the cache size and so needs to set up its own connection.
    # Override the standard methods.
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def ConnectionOpen(self, cacheSize):
        self.home = '.'
        conn_params = 'create,statistics=(fast),' + \
            cacheSize + ',error_prefix="%s" %s' % (self.shortid(), self.conn_config)
        try:
            self.conn = wiredtiger.wiredtiger_open(self.home, conn_params)
        except wiredtiger.WiredTigerError as e:
            print("Failed conn at '%s' with config '%s'" % (dir, conn_params))
        self.session = wttest.WiredTigerTestCase.setUpSessionOpen(self, self.conn)

    def test_rollback_to_stable(self):
        self.ConnectionOpen(self.cacheSize)
        # Configure small page sizes to ensure eviction comes through and we
        # have a somewhat complex tree.
        config_default = 'key_format=i,value_format=i,memory_page_max=32k,leaf_page_max=8k,internal_page_max=8k'
        config_nolog   = ',log=(enabled=false)'
        #
        # Open four tables:
        # 1. Table is logged and uses timestamps.
        # 2. Table is not logged and uses timestamps.
        # 3. Table is logged and does not use timestamps.
        # 4. Table is not logged and does not use timestamps.
        #
        self.session.create(self.table_ts_log, config_default + self.extra_config)
        cur_ts_log = self.session.open_cursor(self.table_ts_log)
        self.session.create(self.table_ts_nolog, config_default + config_nolog + self.extra_config)
        cur_ts_nolog = self.session.open_cursor(self.table_ts_nolog)
        self.session.create(self.table_nots_log, config_default + self.extra_config)
        cur_nots_log = self.session.open_cursor(self.table_nots_log)
        self.session.create(self.table_nots_nolog, config_default + config_nolog + self.extra_config)
        cur_nots_nolog = self.session.open_cursor(self.table_nots_nolog)

        # Insert keys each with timestamp=key, in some order.
        key_range = 10000
        keys = list(range(1, key_range + 1))

        # Set keys 1-key_range to value 1.
        for k in keys:
            cur_nots_log[k] = 1
            cur_nots_nolog[k] = 1
            self.session.begin_transaction()
            cur_ts_log[k] = 1
            cur_ts_nolog[k] = 1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(k))
            # Setup an oldest timestamp to ensure state remains in cache.
            if k == 1:
                self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cur_ts_log.close()
        cur_ts_nolog.close()
        cur_nots_log.close()
        cur_nots_nolog.close()

        # Scenario: 1
        # Check that we see all the inserted values(i.e 1) in all tables
        latest_ts = self.timestamp_str(key_range)
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_log, dict((k, 1) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_nolog, dict((k, 1) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_log, dict((k, 1) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 1) for k in keys[:]))

        # Scenario: 2
        # Roll back half timestamps.
        stable_ts = self.timestamp_str(key_range // 2)
        self.conn.set_timestamp('stable_timestamp=' + stable_ts)

        # We're about to test rollback-to-stable which requires a checkpoint to which we can roll back.
        self.session.checkpoint()
        self.conn.rollback_to_stable()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = (stat_cursor[stat.conn.txn_rts_upd_aborted][2] +
            stat_cursor[stat.conn.txn_rts_hs_removed][2] +
            stat_cursor[stat.conn.txn_rts_keys_removed][2])
        stat_cursor.close()
        self.assertEqual(calls, 1)
        self.assertTrue(upd_aborted >= key_range/2)

        # Check that we see the inserted value (i.e. 1) for all the keys in
        # non-timestamp tables.
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_log, dict((k, 1) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_nolog, dict((k, 1) for k in keys[:]))

        # For non-logged tables the behavior is consistent across connections
        # with or without log enabled.
        # Check that we see the inserted value (i.e. 1) for the keys in a
        # timestamped table until the stable_timestamp only.
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 1) for k in keys[:(key_range // 2)]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 1) for k in keys[(key_range // 2 + 1):]), missing=True)

        # For logged tables, the behavior of rollback_to_stable changes based on
        # whether connection level logging is enabled or not.
        if self.using_log == True:
            # When the log is enabled, none of the keys will be rolled back.
            # Check that we see all the keys.
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, 1) for k in keys[:]))
        else:
            # When the log is disabled, the keys will be rolled back until stable_timestamp.
            # Check that we see the insertions are rolled back in timestamped tables
            # until the stable_timestamp.
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, 1) for k in keys[:(key_range // 2)]))
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, 1) for k in keys[(key_range // 2 + 1):]), missing=True)

        # Bump the oldest timestamp, we're not going back.
        self.conn.set_timestamp('oldest_timestamp=' + stable_ts)

        # Update the values again in preparation for rolling back more.
        cur_ts_log = self.session.open_cursor(self.table_ts_log)
        cur_ts_nolog = self.session.open_cursor(self.table_ts_nolog)
        cur_nots_log = self.session.open_cursor(self.table_nots_log)
        cur_nots_nolog = self.session.open_cursor(self.table_nots_nolog)
        for k in keys:
            cur_nots_log[k] = 2
            cur_nots_nolog[k] = 2
            self.session.begin_transaction()
            cur_ts_log[k] = 2
            cur_ts_nolog[k] = 2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(k + key_range))
        cur_ts_log.close()
        cur_ts_nolog.close()
        cur_nots_log.close()
        cur_nots_nolog.close()

        # Scenario: 3
        # Check that we see all values updated (i.e 2) in all tables.
        latest_ts = self.timestamp_str(2 * key_range)
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_log, dict((k, 2) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_nolog, dict((k, 2) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_log, dict((k, 2) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 2) for k in keys[:]))

        # Scenario: 4
        # Advance the stable_timestamp by a quarter range and rollback.
        # Three-fourths of the later timestamps will be rolled back.
        rolled_range = key_range + key_range // 4
        stable_ts = self.timestamp_str(rolled_range)
        self.conn.set_timestamp('stable_timestamp=' + stable_ts)
        self.conn.rollback_to_stable()
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = (stat_cursor[stat.conn.txn_rts_upd_aborted][2] +
            stat_cursor[stat.conn.txn_rts_hs_removed][2] +
            stat_cursor[stat.conn.txn_rts_keys_removed][2])
        stat_cursor.close()
        self.assertEqual(calls, 2)

        #
        # We rolled back half on the earlier call and now three-quarters on
        # this call, which is one and one quarter of all keys rolled back.
        #
        self.assertTrue(upd_aborted >= rolled_range)

        # Check that we see the updated value (i.e. 2) for all the keys in
        # non-timestamped tables.
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_log, dict((k, 2) for k in keys[:]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_nots_nolog, dict((k, 2) for k in keys[:]))
        # For non-logged tables the behavior is consistent across connections
        # with or without log enabled.
        # Check that we see only half key ranges in timestamp tables. We see
        # the updated value (i.e. 2) for the first quarter keys and old values
        # (i.e. 1) for the second quarter keys.
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 2 if k <= key_range // 4 else 1)
            for k in keys[:(key_range // 2)]))
        self.check(self.session, 'read_timestamp=' + latest_ts,
            self.table_ts_nolog, dict((k, 1) for k in keys[(1 + key_range // 2):]), missing=True)

        # For logged tables behavior changes for rollback_to_stable based on
        # whether connection level logging is enabled or not.
        if self.using_log == True:
            # When log is enabled, none of the keys will be rolled back.
            # Check that we see all the keys.
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, 2) for k in keys[:]))
        else:
            # When log is disabled, keys will be rolled back until the stable_timestamp.
            # Check that we see only half the key ranges in timestamped tables. We see
            # the updated value (i.e. 2) for the first quarter keys and old values
            # (i.e. 1) for the second quarter keys.
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, (2 if k <= key_range // 4 else 1))
                for k in keys[:(key_range // 2)]))
            self.check(self.session, 'read_timestamp=' + latest_ts,
                self.table_ts_log, dict((k, 1) for k in keys[(1 + key_range // 2):]), missing=True)

if __name__ == '__main__':
    wttest.run()
