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

import time
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# Test that a thread resolving a transaction is released from the eviction assist at its bounded
# wait. The assist normally spins until the cache drops below its triggers, which never happens when
# the dirty content cannot be reconciled away - here because another session is holding it
# uncommitted. The resolving thread pins no transaction state, so nothing can roll it back to
# relieve the pressure and it must be released on its own. The bound is what remains of the caller's
# own operation timeout, so the test configures a short one rather than waiting out the default cap.
class test_eviction07(wttest.WiredTigerTestCase):
    uri = 'table:test_eviction07'
    cache_bytes = 50 * 1024 * 1024
    dirty_trigger_pct = 5
    operation_timeout_ms = 500

    resolution_values = [
        ('commit', dict(rollback=False)),
        ('rollback', dict(rollback=True)),
    ]
    scenarios = make_scenarios(resolution_values)

    conn_config = 'cache_size=50MB,statistics=(all),' \
        'eviction_dirty_target=1,eviction_dirty_trigger=5,eviction=(threads_max=1)'

    def _pin_dirty_content(self, pin_session, pin_cursor):
        # Hold dirty content above the trigger in an uncommitted transaction. Reconciliation has to
        # restore these updates to the page, so eviction cannot reclaim them.
        pin_session.begin_transaction()
        value = 'a' * 4096
        for i in range(1500):
            pin_cursor[i] = value

    def _resolve_until_bounded_wait(self, cursor, stat_session):
        # Resolve modified transactions while the pinned transaction prevents eviction from
        # reducing the dirty cache pressure. The bounded-wait statistic increasing across a
        # resolution proves that the commit or rollback stopped assisting at its time limit.
        value = 'a' * 4096
        bounded_resolution_time = None
        for i in range(100000, 100500):
            # The assist bounds itself by what is left of the operation timeout, so give the
            # resolution a short one instead of waiting out the much larger default cap.
            self.session.begin_transaction(
                'operation_timeout_ms=%d' % self.operation_timeout_ms)
            cursor[i] = value

            bounded_waits = self.get_stat(
                stat.conn.eviction_app_bounded_wait_exceeded, session=stat_session)
            start = time.monotonic()
            if self.rollback:
                self.session.rollback_transaction()
            else:
                self.session.commit_transaction()
            elapsed = time.monotonic() - start

            bounded_waits_after = self.get_stat(
                stat.conn.eviction_app_bounded_wait_exceeded, session=stat_session)
            if bounded_waits_after > bounded_waits:
                bounded_resolution_time = elapsed
                break
        return bounded_resolution_time

    def test_bounded_assist_at_transaction_resolution(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        stat_session = pin_session = None
        pin_cursor = cursor = None
        pin_txn_active = resolution_txn_active = False

        try:
            # Reading statistics is a cursor operation that can itself be pulled into an eviction
            # assist, so read them from a session that never waits for the cache.
            stat_session = self.conn.open_session('cache_max_wait_ms=1')

            pin_session = self.conn.open_session()
            pin_cursor = pin_session.open_cursor(self.uri)
            self._pin_dirty_content(pin_session, pin_cursor)
            pin_txn_active = True

            dirty_trigger = self.cache_bytes * self.dirty_trigger_pct // 100
            dirty = self.get_stat(stat.conn.cache_bytes_dirty, session=stat_session)
            self.assertGreater(dirty, dirty_trigger)

            cursor = self.session.open_cursor(self.uri)
            resolution_txn_active = True
            bounded_resolution_time = self._resolve_until_bounded_wait(cursor, stat_session)
            resolution_txn_active = False

            self.assertIsNotNone(bounded_resolution_time)
            self.assertLess(bounded_resolution_time, 1.0)

            # The pressure must still be there, otherwise the assist stopped because the cache
            # drained.
            dirty = self.get_stat(stat.conn.cache_bytes_dirty, session=stat_session)
            self.assertGreater(dirty, dirty_trigger)
        finally:
            if pin_txn_active:
                pin_session.rollback_transaction()
            if resolution_txn_active:
                self.session.rollback_transaction()
            if cursor is not None:
                cursor.close()
            if pin_cursor is not None:
                pin_cursor.close()
            if pin_session is not None:
                pin_session.close()
            if stat_session is not None:
                stat_session.close()

if __name__ == '__main__':
    wttest.run()
