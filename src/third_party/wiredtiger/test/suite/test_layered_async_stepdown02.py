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

import random
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# test_layered_async_stepdown02.py
#    Read semantics: iteration across the step-down timestamp, merged lookups, a per-timestamp
#    oracle and a randomized stress phase.
@disagg_test_class
class test_layered_async_stepdown02(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # A scan interrupted by the step-down timestamp re-seats and still yields its own snapshot
    # exactly once, in order, whatever the concurrent writer does to the keys underneath it.
    def test_iteration_across_step_down_ts(self):
        uri = f'layered:{self.test_name}_iter'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        # Even-numbered keys form the stable content the scan snapshot will see.
        stable_keys = [f'k{i:02d}' for i in range(0, 20, 2)]
        self.write_at(uri, {k: 'v' for k in stable_keys}, 10)

        # Use a second session for the concurrent writer so the scan's transaction stays untouched.
        wsession = self.conn.open_session()

        # Read below the concurrent writer's later commit.
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))

        # Walk part way, then set the timestamp mid-iteration.
        seen = []
        for _ in range(4):
            self.assertEqual(cursor.next(), 0)
            seen.append(cursor.get_key())
        self.set_step_down_ts(50)

        # The concurrent transaction interleaves new odd-numbered keys into ingest both behind and
        # ahead of the scan position, and also updates and removes stable keys the scan has not
        # reached yet: an invisible update or tombstone must not disturb the walk either.
        updated = stable_keys[6]
        removed = stable_keys[8]
        wcur = wsession.open_cursor(uri, None, None)
        wsession.begin_transaction()
        for i in range(1, 20, 2):
            wcur[f'k{i:02d}'] = 'ingest'
        wcur[updated] = 'ingest-update'
        wcur.set_key(removed)
        self.assertEqual(wcur.remove(), 0)
        wsession.commit_transaction('commit_timestamp=' + self.timestamp_str(60))
        wcur.close()
        wsession.close()

        # Finish the walk. Setting the step-down timestamp forces a re-seat; none of the ingest
        # records are visible to the scan's snapshot, so the rest must still come back in order
        # with no duplicates, and the shadowed keys must keep their stable values.
        kvs = []
        while cursor.next() == 0:
            seen.append(cursor.get_key())
            kvs.append((cursor.get_key(), cursor.get_value()))
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(seen, stable_keys,
            'the scan must yield exactly the snapshot keys once, in order')
        self.assertIn((updated, 'v'), kvs, 'the invisible update must not reach this snapshot')
        self.assertIn((removed, 'v'), kvs, 'the invisible tombstone must not reach this snapshot')

        # A fresh scan above the ingest commit sees the merge: interleaved keys, the update
        # applied and the removed key gone.
        expected = {f'k{i:02d}': 'ingest' for i in range(1, 20, 2)}
        expected.update({k: 'v' for k in stable_keys})
        expected[updated] = 'ingest-update'
        del expected[removed]
        self.assertEqual(self.read_kvs_at(uri, 70), expected)

    # Point/range lookups merge ingest over stable.
    def test_search_and_search_near_merged(self):
        uri = f'layered:{self.test_name}_search'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        # These keys go to stable; the interleaved ones written later go to ingest.
        self.write_at(uri, {'b': 's', 'd': 's', 'f': 's'}, 10)
        self.set_step_down_ts(20)
        self.write_at(uri, {'a': 'i', 'c': 'i', 'e': 'i'}, 30)
        self.assertEqual(self.read_keys_at(self.stable_uri(uri), 40), {'b', 'd', 'f'})

        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))

        # Exact search finds keys from both constituents.
        self.assertEqual(cursor['c'], 'i')
        self.assertEqual(cursor['d'], 's')

        # A miss is a miss across the merged view.
        cursor.set_key('z')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

        # search_near on a non-existent key positions on an adjacent key from either constituent.
        cursor.set_key('cc')
        cmp = cursor.search_near()
        self.assertNotEqual(cmp, wiredtiger.WT_NOTFOUND)
        self.assertIn(cursor.get_key(), ('c', 'd'))

        # Full merged order interleaves the two constituents.
        cursor.reset()
        order = []
        while cursor.next() == 0:
            order.append(cursor.get_key())
        self.assertEqual(order, ['a', 'b', 'c', 'd', 'e', 'f'])
        self.session.rollback_transaction()
        cursor.close()

    # A write to ingest is visible to a later read in the same transaction.
    def test_read_your_own_writes_after_step_down_ts(self):
        uri = f'layered:{self.test_name}_ryow'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')
        self.write_at(uri, {'old': 'stable'}, 10)

        self.set_step_down_ts(20)

        wcur = self.session.open_cursor(uri, None, None)
        rcur = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        wcur['fresh'] = 'ingest'
        wcur['old'] = 'ingest'
        # Same transaction sees its own ingest writes merged over stable.
        self.assertEqual(rcur['fresh'], 'ingest')
        self.assertEqual(rcur['old'], 'ingest')
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        wcur.close()
        rcur.close()

        self.assertEqual(self.read_kvs_at(uri, 40), {'old': 'ingest', 'fresh': 'ingest'})

        # Ground truth: both writes landed in ingest; the stable version is untouched.
        self.assertEqual(self.read_kvs_at(self.stable_uri(uri), 40), {'old': 'stable'})

    # Reverse iteration and largest_key on either side of the step-down timestamp; largest_key is
    # non-transactional.
    def test_prev_and_largest_key_across_step_down_ts(self):
        uri = f'layered:{self.test_name}_revscan'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')
        self.write_at(uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        def reverse_keys(read_ts):
            c = self.session.open_cursor(uri, None, None)
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
            keys = []
            while c.prev() == 0:
                keys.append(c.get_key())
            self.session.rollback_transaction()
            c.close()
            return keys

        # largest_key ignores visibility, so no transaction is needed.
        def largest():
            c = self.session.open_cursor(uri, None, None)
            self.assertEqual(c.largest_key(), 0)
            key = c.get_key()
            c.close()
            return key

        # Stable only.
        self.assertEqual(reverse_keys(15), ['f', 'd', 'b'])
        self.assertEqual(largest(), 'f')

        self.set_step_down_ts(20)
        # The merged maximum lives in ingest.
        self.write_at(uri, {'a': 'i', 'c': 'i', 'e': 'i', 'z': 'i'}, 30)

        # Reverse merged order across both constituents.
        self.assertEqual(reverse_keys(40), ['z', 'f', 'e', 'd', 'c', 'b', 'a'])
        self.assertEqual(largest(), 'z')

    # Read ops through straddling reader: snapshot pins stable; ingest invisible except largest_key.
    def test_read_ops_across_step_down_ts(self):
        uri = f'layered:{self.test_name}_readops'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')
        self.write_at(uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        rcur = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        self.assertEqual(rcur['d'], 's')

        self.set_step_down_ts(20)

        # A concurrent transaction interleaves ingest keys, including a new maximum.
        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(uri, None, None)
        wsession.begin_transaction()
        for k in ('a', 'c', 'e', 'z'):
            wcur[k] = 'i'
        wsession.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        wcur.close()
        wsession.close()

        # search: stable hit still works, the invisible ingest key is a miss.
        self.assertEqual(rcur['d'], 's')
        rcur.set_key('a')
        self.assertEqual(rcur.search(), wiredtiger.WT_NOTFOUND)

        # search_near: lands on a visible stable neighbor, never the invisible ingest 'c'.
        rcur.set_key('c')
        cmp = rcur.search_near()
        self.assertNotEqual(cmp, wiredtiger.WT_NOTFOUND)
        self.assertIn(rcur.get_key(), ('b', 'd'))

        # prev: the full reverse walk yields exactly the snapshot's keys.
        rcur.reset()
        seen = []
        while rcur.prev() == 0:
            seen.append(rcur.get_key())
        self.assertEqual(seen, ['f', 'd', 'b'])

        # largest_key ignores visibility: it reports the ingest maximum even though this
        # snapshot cannot read it.
        self.assertEqual(rcur.largest_key(), 0)
        self.assertEqual(rcur.get_key(), 'z')

        self.session.rollback_transaction()
        rcur.close()

    # Check every read op against a per-timestamp oracle: tombstone/re-insert/straddler merges.
    def test_oracle_reads_merges(self):
        uri = f'layered:{self.test_name}_oracle'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        universe = {'gone', 'reborn', 'upd', 'keep', 'straddle', 'new'}
        cursor = self.session.open_cursor(uri, None, None)

        # Stable phase: four keys at 10, then 'reborn' is deleted in stable at 12.
        self.write_at(uri, {'gone': 's', 'reborn': 's', 'upd': 's', 'keep': 's'}, 10)
        self.remove_at(uri, ['reborn'], 12)

        # A straddler writes beforehand and rolls back: 'straddle' must leave no trace.
        self.session.begin_transaction()
        cursor['straddle'] = 'never'
        self.set_step_down_ts(20)
        self.assert_step_down_rollback(
            lambda: self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25)))
        cursor.close()

        # Ingest phase: a tombstone over the stable value of 'gone', a re-insert of the key deleted
        # in stable, an overwrite of a stable value, and a key that never existed in stable at all.
        self.remove_at(uri, ['gone'], 30)
        self.write_at(uri, {'reborn': 'i'}, 35)
        self.write_at(uri, {'upd': 'i2'}, 40)
        self.write_at(uri, {'new': 'i'}, 45)

        oracle = {
            10: {'gone': 's', 'reborn': 's', 'upd': 's', 'keep': 's'},
            12: {'gone': 's', 'upd': 's', 'keep': 's'},
            25: {'gone': 's', 'upd': 's', 'keep': 's'},
            30: {'upd': 's', 'keep': 's'},
            35: {'reborn': 'i', 'upd': 's', 'keep': 's'},
            40: {'reborn': 'i', 'upd': 'i2', 'keep': 's'},
            45: {'reborn': 'i', 'upd': 'i2', 'keep': 's', 'new': 'i'},
        }

        # Verify every read op against the oracle at every timestamp: full forward scan, point
        # reads over the whole key universe, and a reverse scan.
        def check_oracle(phase):
            for ts, expected in oracle.items():
                ctx = f'{phase} read_ts={ts}'
                self.assertEqual(self.read_kvs_at(uri, ts), expected, f'scan mismatch: {ctx}')

                rc = self.session.open_cursor(uri, None, None)
                self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
                for k in sorted(universe):
                    rc.set_key(k)
                    if k in expected:
                        self.assertEqual(rc.search(), 0, f'expected hit: {ctx} key={k}')
                        self.assertEqual(rc.get_value(), expected[k],
                            f'value mismatch: {ctx} key={k}')
                    else:
                        self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND,
                            f'expected miss: {ctx} key={k}')
                rc.reset()
                rev = []
                while rc.prev() == 0:
                    rev.append(rc.get_key())
                self.assertEqual(rev, sorted(expected.keys(), reverse=True),
                    f'reverse scan mismatch: {ctx}')
                self.session.rollback_transaction()
                rc.close()

        check_oracle('leader')

        # Ground truth: nothing in the ingest phase touched stable; the remove of 'gone' is a
        # marker record in ingest that hides the stable value at merge time.
        self.assertEqual(self.read_kvs_at(self.stable_uri(uri), 50),
            {'gone': 's', 'upd': 's', 'keep': 's'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(uri), 50),
            {'gone', 'reborn', 'upd', 'new'})

        # Every timestamp must answer identically after the completed step-down.
        self.complete_step_down(20)
        check_oracle('follower')

    # Randomized ops split either side of the step-down timestamp, with the merged view checked
    # against a shadow map.
    #
    # FIXME-WT-18209: extend the layered cursor stress test to cover async step-down and retire this.
    def test_stress_random_ops(self):
        uri = f'layered:{self.test_name}_stress'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        # The fixed seed keeps the op sequence deterministic.
        seed = 42
        self.pr(f'test_stress_random_ops: random seed {seed}')
        rng = random.Random(seed)
        nkeys = 40
        expected = {}
        self.ts = 1
        cursor = self.session.open_cursor(uri, None, None)

        def rand_key():
            return f'k{rng.randrange(nkeys):02d}'

        # Cross-check a handful of point reads against the expected contents (exercises the merge
        # on search()).
        def check_point_reads(read_ts):
            rc = self.session.open_cursor(uri, None, None)
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
            for _ in range(10):
                k = rand_key()
                ctx = f'seed={seed} read_ts={read_ts} key={k}'
                rc.set_key(k)
                if k in expected:
                    self.assertEqual(rc.search(), 0, f'expected hit: {ctx}')
                    self.assertEqual(rc.get_value(), expected[k], f'value mismatch: {ctx}')
                else:
                    self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND, f'expected miss: {ctx}')
            self.session.rollback_transaction()
            rc.close()

        def run_ops(n, verify_every=0):
            for i in range(n):
                self.ts += 1
                k = rand_key()
                roll = rng.random()
                self.session.begin_transaction()
                if roll < 0.55:
                    # Insert or overwrite.
                    v = f'v{self.ts}'
                    cursor[k] = v
                    expected[k] = v
                elif roll < 0.75 and k in expected:
                    # Modify: replace the first byte, built on the current value.
                    cursor.set_key(k)
                    cursor.modify([wiredtiger.Modify('Z', 0, 1)])
                    expected[k] = 'Z' + expected[k][1:]
                elif k in expected:
                    # Remove an existing key.
                    cursor.set_key(k)
                    self.assertEqual(cursor.remove(), 0)
                    del expected[k]
                else:
                    # Nothing to modify/remove; make it an insert instead.
                    v = f'v{self.ts}'
                    cursor[k] = v
                    expected[k] = v
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.ts))
                if verify_every and (i + 1) % verify_every == 0:
                    self.assertEqual(self.read_kvs_at(uri, self.ts), dict(expected),
                        f'table does not match expected: seed={seed} op={i + 1} ts={self.ts}')
                    check_point_reads(self.ts)

        # Phase 1: churn before the step-down timestamp, everything routed to stable.
        run_ops(120, verify_every=40)
        self.assertEqual(self.read_kvs_at(uri, self.ts), dict(expected))
        snapshot_ts = self.ts
        snapshot = dict(expected)

        # Set the cutoff at the current frontier (the last committed timestamp), so every later
        # commit sits strictly above it. Phase 2: churn routed to ingest.
        self.set_step_down_ts(self.ts)
        run_ops(120, verify_every=40)

        # The merged view reflects every operation across both constituents.
        self.assertEqual(self.read_kvs_at(uri, self.ts), dict(expected),
            'the merged view must match the expected contents')
        check_point_reads(self.ts)

        # Time-travel: the view at that boundary is unchanged by the later ingest writes.
        self.assertEqual(self.read_kvs_at(uri, snapshot_ts), snapshot,
            'reading at the old frontier must be unaffected by the later writes')

        # Ground truth: the churn afterwards never touched the stable table.
        self.assertEqual(self.read_kvs_at(self.stable_uri(uri), self.ts), snapshot,
            'later writes must not leak into the stable table')

        cursor.close()

        # The merged view and point reads survive the completed step-down.
        self.complete_step_down(snapshot_ts)
        self.assertEqual(self.read_kvs_at(uri, self.ts), dict(expected),
            'merged layered view must match the expected contents after the step-down')
        check_point_reads(self.ts)
