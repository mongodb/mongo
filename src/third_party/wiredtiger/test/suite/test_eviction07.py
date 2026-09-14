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

# Test that a thread resolving a transaction is released from eviction assist when dirty content
# cannot be reclaimed. With an operation timeout, the remaining timeout ends the assist. Without
# one, the default bounded wait does. Another session holds the dirty content uncommitted, so
# eviction cannot drain the cache and the resolving thread pins no transaction state.
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

    def _pin_dirty_content(self, pin_sessions_and_cursors):
        # Hold dirty content above the trigger across several uncommitted transactions.
        # Reconciliation has to restore these updates to the page, so eviction cannot reclaim them.
        # No single transaction's own dirty content may exceed the (lower of the updates or dirty)
        # trigger, or it becomes a candidate for rollback in its own right rather than a page
        # eviction cannot reconcile.
        value = 'a' * 4096
        rows_per_txn = 200
        for txn_num, (pin_session, pin_cursor) in enumerate(pin_sessions_and_cursors):
            pin_session.begin_transaction()
            base = txn_num * rows_per_txn
            for i in range(rows_per_txn):
                pin_cursor[base + i] = value

    def _resolve_transaction(self, cursor, config=None):
        if config is None:
            self.session.begin_transaction()
        else:
            self.session.begin_transaction(config)
        cursor[self._next_key] = 'a' * 4096
        self._next_key += 1
        start = time.monotonic()
        if self.rollback:
            self.session.rollback_transaction()
        else:
            self.session.commit_transaction()
        return time.monotonic() - start

    def _resolve_until_timeout(self, cursor, stat_session):
        # A resolution that takes a material fraction of the operation timeout, but still returns
        # well under the default assist cap, shows the caller's timeout was honored. The
        # bounded-wait statistic must not increase across that resolution: that path is the
        # fallback when no operation timeout is set.
        #
        # Both deadlines can expire in the same assist iteration, so a single increment is
        # retried. If every slow resolution increments, the operation timeout is not what
        # released the thread.
        min_elapsed = self.operation_timeout_ms / 1000.0 * 0.4
        slow_with_bounded_wait = 0
        timeout_config = 'operation_timeout_ms=%d' % self.operation_timeout_ms
        for _ in range(500):
            bounded_waits = self.get_stat(
                stat.conn.eviction_app_bounded_wait_exceeded, session=stat_session)
            elapsed = self._resolve_transaction(cursor, timeout_config)
            if elapsed < min_elapsed:
                continue
            if self.get_stat(stat.conn.eviction_app_bounded_wait_exceeded,
                session=stat_session) == bounded_waits:
                return elapsed
            slow_with_bounded_wait += 1
            if slow_with_bounded_wait >= 3:
                return None
        return None

    def _resolve_until_bounded_wait(self, cursor, stat_session):
        # No operation timeout, so the assist uses the default bounded wait. The statistic
        # increasing across a resolution proves that path released the thread.
        for _ in range(500):
            bounded_waits = self.get_stat(
                stat.conn.eviction_app_bounded_wait_exceeded, session=stat_session)
            elapsed = self._resolve_transaction(cursor)
            if self.get_stat(stat.conn.eviction_app_bounded_wait_exceeded,
                session=stat_session) > bounded_waits:
                return elapsed
        return None

    def _run_with_pinned_dirty(self, resolve):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self._next_key = 100000
        stat_session = None
        pin_sessions_and_cursors = []
        cursor = None
        pin_txns_active = resolution_txn_active = False

        try:
            # Reading statistics is a cursor operation that can itself be pulled into an eviction
            # assist, so read them from a session that ignores the cache size. Pin sessions ignore
            # it too: they only hold uncommitted dirty, and rolling them back must not wait out
            # the default bounded assist.
            stat_session = self.conn.open_session('ignore_cache_size=true')

            for _ in range(8):
                pin_session = self.conn.open_session('ignore_cache_size=true')
                pin_sessions_and_cursors.append(
                    (pin_session, pin_session.open_cursor(self.uri)))
            self._pin_dirty_content(pin_sessions_and_cursors)
            pin_txns_active = True

            dirty_trigger = self.cache_bytes * self.dirty_trigger_pct // 100
            dirty = self.get_stat(stat.conn.cache_bytes_dirty, session=stat_session)
            self.assertGreater(dirty, dirty_trigger)

            cursor = self.session.open_cursor(self.uri)
            resolution_txn_active = True
            resolution_time = resolve(cursor, stat_session)
            resolution_txn_active = False

            # The pressure must still be there, otherwise the assist stopped because the cache
            # drained.
            dirty = self.get_stat(stat.conn.cache_bytes_dirty, session=stat_session)
            self.assertGreater(dirty, dirty_trigger)
            return resolution_time
        finally:
            if pin_txns_active:
                for pin_session, _ in pin_sessions_and_cursors:
                    pin_session.rollback_transaction()
            if resolution_txn_active:
                self.session.rollback_transaction()
            if cursor is not None:
                cursor.close()
            for pin_session, pin_cursor in pin_sessions_and_cursors:
                pin_cursor.close()
                pin_session.close()
            if stat_session is not None:
                stat_session.close()

    def test_operation_timeout_at_transaction_resolution(self):
        resolution_time = self._run_with_pinned_dirty(self._resolve_until_timeout)
        self.assertIsNotNone(resolution_time)
        self.assertLess(resolution_time, 1.0)

    def test_bounded_assist_at_transaction_resolution(self):
        resolution_time = self._run_with_pinned_dirty(self._resolve_until_bounded_wait)
        self.assertIsNotNone(resolution_time)
        self.assertGreater(resolution_time, 5.0)
        self.assertLess(resolution_time, 90.0)

if __name__ == '__main__':
    wttest.run()
