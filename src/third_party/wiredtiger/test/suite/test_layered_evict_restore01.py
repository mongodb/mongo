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

import time, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# Eviction must not spin re-restoring the same disaggregated leaf pages when the
# updates they hold cannot become evictable.
#
# Scenario:
#   1. A leader commits updates with timestamps above the stable timestamp and
#      then goes idle, so no transaction IDs are outstanding.
#   2. The updates trigger is lowered below the update bytes now in cache.
#   3. Eviction can only rewrite those pages with the same updates restored: they
#      stay above the trigger and eviction makes no progress, so it turns
#      aggressive.
#   4. Nothing that could make the updates evictable changes, so re-evicting a
#      restored page cannot help. Eviction must skip the restored pages instead
#      of re-queuing them on every walk.
#   5. Once stable advances, the skipped pages must be evicted again and the
#      update bytes drain.
#
# A second scenario keeps stable pinned and instead holds the updates in one
# long-running transaction. Resolving that transaction must also make the
# skipped pages evictable again, even though stable never moves.
@disagg_test_class
class test_layered_evict_restore01(wttest.WiredTigerTestCase):
    test_name = __qualname__

    # A stuck-cache timeout would abort the process in diagnostic builds; the
    # loop is observed through statistics instead.
    conn_base_config = 'cache_size=100MB,statistics=(all),cache_stuck_timeout_ms=0,precise_checkpoint=true,' \
        'eviction_dirty_target=40,eviction_dirty_trigger=60,' \
        'eviction_updates_target=40,eviction_updates_trigger=50,'

    disagg_storages = gen_disagg_storages(disagg_only = True)
    scenarios = make_scenarios(disagg_storages)
    uri = f'layered:{test_name}'

    nrows = 20000
    value = 'v' * 1024
    idle_secs = 5

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader"),'

    def sample_stats(self, *keys):
        c = self.session.open_cursor('statistics:')
        vals = [c[k][2] for k in keys]
        c.close()
        return vals

    def wait_aggressive(self):
        self.assertStatGreaterSoon(stat.conn.eviction_aggressive_set, 0, timeout=60,
            msg='eviction never became aggressive')

    def wait_updates_drain(self, what):
        cache_max = self.get_stat(stat.conn.cache_bytes_max)
        deadline = time.time() + 60
        while True:
            updates = self.get_stat(stat.conn.cache_bytes_updates)
            if updates < cache_max * 5 // 100:
                break
            self.assertLess(time.time(), deadline,
                f'update bytes did not drain after {what}: {updates}')
            time.sleep(0.1)

    def test_restore_loop_skips_restored_pages(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Every update is newer than stable, so none can leave memory.
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            self.session.begin_transaction()
            c[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 10))
        c.close()

        # Push the updates trigger well below the update bytes in cache.
        self.conn.reconfigure('eviction_updates_target=2,eviction_updates_trigger=5')

        # Let eviction escalate to aggressive before sampling.
        self.wait_aggressive()

        keys = (stat.conn.cache_write_restore_invisible,
            stat.conn.eviction_server_skip_pages_restored_unchanged)
        restore0, skip0 = self.sample_stats(*keys)
        time.sleep(self.idle_secs)
        restore1, skip1, updates, cache_max, aggressive = self.sample_stats(*keys,
            stat.conn.cache_bytes_updates, stat.conn.cache_bytes_max,
            stat.conn.eviction_aggressive_set)
        restores = restore1 - restore0
        skips = skip1 - skip0
        self.pr(f'idle {self.idle_secs}s: restores={restores} restored_skips={skips} '
            f'updates={updates} ({100.0 * updates / cache_max:.2f}%) aggressive={aggressive}')

        # The precondition holds: update bytes are still over the trigger and
        # eviction is aggressive.
        self.assertGreater(updates, cache_max * 5 // 100)
        self.assertGreater(aggressive, 0)

        # Restored pages must be skipped rather than re-queued and restored again
        # on every pass.
        self.assertGreater(skips, 0,
            f'restored pages re-queued: {restores} restores in {self.idle_secs}s idle, 0 skips')
        self.assertLess(restores, 1000,
            f'{restores} restores in {self.idle_secs}s idle with stable pinned')

        # Advancing stable must make the skipped pages evictable again.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.nrows + 10))
        self.wait_updates_drain('stable advanced')

    def test_restored_pages_retried_after_txn_resolves(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # One running transaction holds every update and pins the oldest ID.
        txn_session = self.conn.open_session()
        c = txn_session.open_cursor(self.uri)
        txn_session.begin_transaction()
        for i in range(self.nrows):
            c[i] = self.value

        self.conn.reconfigure('eviction_updates_target=2,eviction_updates_trigger=5')
        self.wait_aggressive()

        # With the transaction still running, restored pages are skipped.
        skip_key = stat.conn.eviction_server_skip_pages_restored_unchanged
        self.assertStatGreaterSoon(skip_key, self.get_stat(skip_key), timeout=60,
            msg='restored pages were never skipped')

        restores, updates, cache_max = self.sample_stats(stat.conn.cache_write_restore_invisible,
            stat.conn.cache_bytes_updates, stat.conn.cache_bytes_max)
        self.pr(f'before rollback: restores={restores} updates={updates} '
            f'({100.0 * updates / cache_max:.2f}%)')
        self.assertGreater(restores, 0)
        self.assertGreater(updates, cache_max * 5 // 100)

        # Rolling back advances the oldest ID but not stable. The aborted
        # updates can then be discarded, so the pages must be retried.
        c.close()
        txn_session.rollback_transaction()
        self.wait_updates_drain('the transaction rolled back')
        txn_session.close()
