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

import threading
import time

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat


# Stress the two race windows that made the scan-based statistics in
# test_disagg_fast_truncate03 unreliable, and assert scan correctness throughout:
#
#   - a walk restarted by a concurrent split re-examines pages already skipped.
#   - an emptied internal page still resident in memory can be evicted between
#     the walk's skip check and the page swap, and read back.
#
# Both races are benign, so no statistic is stable under them. What must hold is
# the data: every scan returns exactly the records that survive the truncate.
# Drive the races hard with concurrent splits and let cache pressure cycle the
# emptied internal pages through eviction while scans run.
@disagg_test_class
class test_disagg_fast_truncate04(wttest.WiredTigerTestCase):
    uri = "table:test_disagg_fast_truncate04"
    nrows = 2000
    ninserts = 400
    nscans = 50
    value = "a" * 50
    trunc_start = 200
    trunc_stop = 1800

    truncate_ts = 20
    read_ts = 25
    insert_ts = 40

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return ('cache_size=50MB,statistics=(all),precise_checkpoint=true,'
                'checkpoint_cleanup=(wait=100000),disaggregated=(role="leader"),')

    def read_stat(self, stat_key):
        # Read with statistics=(fast): the default (all) does a full tree walk that
        # reads every fast-deleted leaf back into cache, perturbing the very loop
        # this test observes.
        with wttest.open_cursor(
            self.session, "statistics:" + self.uri, config="statistics=(fast)"
        ) as stat_cursor:
            return stat_cursor[stat_key][2]

    def evict_keys(self, keys):
        # Force-evict the pages holding the given keys.
        with (
            wttest.open_cursor(
                self.session, self.uri, config="debug=(release_evict)"
            ) as evict_cursor,
            self.transaction(rollback=True),
        ):
            for key in keys:
                evict_cursor.set_key(key)
                evict_cursor.search()
                evict_cursor.reset()

    def scan_keys(self):
        # Return every key visible at read_ts, walked left to right.
        keys = []
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(self.read_ts))
        with wttest.open_cursor(self.session, self.uri) as cursor:
            while cursor.next() == 0:
                keys.append(cursor.get_key())
        self.session.rollback_transaction()
        return keys

    def insert_worker(self, errors):
        # Append past the truncated range, splitting the tree the scans walk.
        session = self.conn.open_session()
        try:
            with wttest.open_cursor(session, self.uri) as cursor:
                for i in range(self.ninserts):
                    session.begin_transaction()
                    cursor[self.nrows + 1 + i] = self.value
                    session.commit_transaction(
                        "commit_timestamp=" + self.timestamp_str(self.insert_ts + i))
        except Exception as e:
            errors.append(e)
        finally:
            session.close()

    def test_scan_with_concurrent_splits(self):
        # Cache pressure plus split stress can stall an eviction pass on loaded machines.
        self.ignoreStdoutPatternIfExists('Eviction took more than 1 minute')
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))

        # Small pages give a deep tree where the truncated range covers every child of
        # several non-root internal pages.
        self.session.create(
            self.uri,
            "key_format=i,value_format=S,block_manager=disagg,log=(enabled=false),"
            "allocation_size=512,leaf_page_max=512,internal_page_max=512,"
            "memory_page_max=4096",
        )
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=10),
        ):
            for key in range(1, self.nrows + 1):
                cursor[key] = self.value

        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(10))
        self.session.checkpoint()
        # On-disk leaves satisfy fast-delete eligibility.
        self.evict_keys(range(1, self.nrows + 1))

        # Fast-truncate the middle of the tree, then checkpoint so the emptied internal
        # pages reconcile to proxy cells and become evictable.
        with (
            wttest.open_cursor(self.session, self.uri) as start_cursor,
            wttest.open_cursor(self.session, self.uri) as stop_cursor,
            self.transaction(commit_timestamp=self.truncate_ts),
        ):
            start_cursor.set_key(self.trunc_start)
            stop_cursor.set_key(self.trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)

        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(self.truncate_ts))
        self.session.checkpoint()

        surviving = list(range(1, self.trunc_start)) + list(
            range(self.trunc_stop + 1, self.nrows + 1))

        # Widen the window where a split is visible to a walk mid-descent.
        self.conn.reconfigure("timing_stress_for_test=[split_1,split_2,split_3,split_4]")

        errors = []
        inserter = threading.Thread(target=self.insert_worker, args=(errors,))
        inserter.start()
        try:
            for _ in range(self.nscans):
                self.assertEqual(self.scan_keys(), surviving)
        finally:
            inserter.join()
        self.assertEqual(errors, [])

        # The skip path engages once the emptied internal pages cycle through eviction;
        # the eviction server owns that timing, so poll for it.
        deadline = time.time() + 30
        while self.read_stat(stat.dsrc.cursor_tree_walk_del_internal_page_skip) == 0:
            self.assertEqual(self.scan_keys(), surviving)
            self.assertLess(
                time.time(), deadline, "no deleted internal page was skipped")

        self.conn.set_timestamp(
            "stable_timestamp=" + self.timestamp_str(self.insert_ts + self.ninserts))
        self.session.checkpoint()
        self.assertEqual(self.scan_keys(), surviving)
