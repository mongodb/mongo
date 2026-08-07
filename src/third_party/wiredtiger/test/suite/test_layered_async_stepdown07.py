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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# test_layered_async_stepdown07.py
#    Supplementary coverage: cursor lifecycle across the step-down, search_near and largest_key
#    corners, the step-down checkpoint, and write conflicts between constituents. The straddler
#    operation matrix and the write-conflict cases have their own classes at the end of the file.
@disagg_test_class
class test_layered_async_stepdown07(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # A cursor closed before the step-down and one opened after it serve the same view.
    def test_cursor_close_reopen_within_txn_across_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's'}, 10)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        c1 = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(c1['b'], 's')
        c1.close()

        self.set_step_down_ts(20)

        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(self.uri, None, None)
        wsession.begin_transaction()
        wcur['x'] = 'i'
        wsession.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        wcur.close()
        wsession.close()

        self.complete_step_down(20)

        c2 = self.session.open_cursor(self.uri, None, None)
        seen = {}
        while c2.next() == 0:
            seen[c2.get_key()] = c2.get_value()
        self.assertEqual(seen, {'b': 's', 'd': 's'})
        self.session.commit_transaction()
        c2.close()

    # One cursor handle works for transactions on both sides of the step-down.
    def test_same_cursor_handle_across_step_down_txns(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'b')
        self.session.rollback_transaction()

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'a': 'i', 'z': 'i'}, 30)
        self.complete_step_down(20)

        # Reset at the transaction end, the handle walks the merged view from the start.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        seen = []
        while cursor.next() == 0:
            seen.append(cursor.get_key())
        self.assertEqual(seen, ['a', 'b', 'd', 'f', 'z'])
        cursor.set_key('d')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 's')
        self.session.rollback_transaction()
        cursor.close()

    # Duplicating a layered cursor is unsupported, cutoff or no cutoff.
    def test_dup_positioned_cursor_across_step_down_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        c1 = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        self.assertEqual(c1.next(), 0)
        self.assertEqual(c1.get_key(), 'b')

        with self.expectedStderrPattern('unsupported object operation'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(None, c1, None))

        self.set_step_down_ts(20)

        with self.expectedStderrPattern('unsupported object operation'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(None, c1, None))

        # The original cursor survives the rejected duplication.
        self.assertEqual(c1.get_key(), 'b')
        self.assertEqual(c1.next(), 0)
        self.assertEqual(c1.get_key(), 'd')

        self.session.rollback_transaction()
        c1.close()

    # Visibility flips at exactly the cutoff after the completed step-down.
    def test_boundary_reads_at_cutoff(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        self.write_at(self.uri, {'below': 'v'}, 19)
        self.write_at(self.uri, {'at': 'v'}, 20)
        self.set_step_down_ts(20)
        self.write_at(self.uri, {'above': 'v'}, 21)
        self.complete_step_down(20)

        self.assertEqual(self.read_kvs_at(self.uri, 19), {'below': 'v'})
        self.assertEqual(self.read_kvs_at(self.uri, 20), {'below': 'v', 'at': 'v'})
        self.assertEqual(self.read_kvs_at(self.uri, 21), {'below': 'v', 'at': 'v', 'above': 'v'})

        # Ground truth: the content split exactly at the cutoff.
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 30), {'below', 'at'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 30), {'above'})

    # Set up a straddler's uncommitted delete on stable and probe it with a later remove of the
    # same key, which routes to ingest with no shared update chain.
    def probe_remove_against_straddler_delete(self, read_config=None):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['victim'] = 'alive'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # The uncommitted tombstone sits on the stable chain.
        straddler_session = self.conn.open_session()
        straddler_cursor = straddler_session.open_cursor(self.uri, None, None)
        straddler_session.begin_transaction()
        straddler_cursor.set_key('victim')
        self.assertEqual(straddler_cursor.remove(), 0)

        self.set_step_down_ts(20)

        # The probing snapshot excludes the straddler, so the conflict check must reach across to
        # the uncommitted delete on stable.
        self.session.begin_transaction(read_config)
        cursor.set_key('victim')
        self.expect_conflict_rollback(cursor.remove)
        self.session.rollback_transaction()

        return cursor, straddler_session, straddler_cursor

    # The conflict is caught with no read timestamp, and once the straddler is gone the retry
    # commits into ingest.
    def test_remove_conflicts_with_uncommitted_straddler_delete(self):
        cursor, straddler_session, straddler_cursor = \
            self.probe_remove_against_straddler_delete()

        # The straddler dies at commit.
        self.assert_step_down_rollback(lambda: straddler_session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(15)), session=straddler_session)
        straddler_cursor.close()
        straddler_session.close()

        # With the straddler resolved, the retry commits into ingest.
        self.session.begin_transaction()
        cursor.set_key('victim')
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Exactly one delete happened.
        self.assertEqual(self.read_kvs_at(self.uri, 25), {'victim': 'alive'})
        self.assertEqual(self.read_kvs_at(self.uri, 35), {})

    # The same conflict with a read timestamp stays caught.
    def test_remove_conflicts_with_read_timestamp(self):
        cursor, straddler_session, straddler_cursor = \
            self.probe_remove_against_straddler_delete(
                'read_timestamp=' + self.timestamp_str(15))
        cursor.close()

        straddler_session.rollback_transaction()
        straddler_cursor.close()
        straddler_session.close()

    # search_near on an exact match reports equality, whichever constituent holds the key, and a
    # read timestamp below the cutoff narrows it to the stable half.
    def test_search_near_exact_and_read_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'a': 'i', 'c': 'i', 'e': 'i'}, 30)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        for key, value in (('d', 's'), ('c', 'i')):
            cursor.set_key(key)
            self.assertEqual(cursor.search_near(), 0, f'exact match expected for {key}')
            self.assertEqual(cursor.get_key(), key)
            self.assertEqual(cursor.get_value(), value)
        self.session.rollback_transaction()

        # Below the cutoff only the stable half is visible, so a key that exists in ingest is no
        # longer an exact match and search_near falls to a stable neighbor.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        cursor.set_key('d')
        self.assertEqual(cursor.search_near(), 0)
        cursor.set_key('c')
        self.assertIn(cursor.search_near(), (-1, 1))
        self.assertIn(cursor.get_key(), ('b', 'd'))
        self.session.rollback_transaction()
        cursor.close()

    # search_near works when only one constituent has content: all of it in ingest over an empty
    # stable table, and nothing anywhere.
    def test_search_near_with_empty_constituent(self):
        empty_uri = f'layered:{self.test_name}_empty'
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session.create(empty_uri, 'key_format=S,value_format=S')

        self.set_step_down_ts(20)

        # Every key lives in ingest; the stable table was never written.
        self.write_at(self.uri, {'b': 'i', 'd': 'i'}, 30)
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), set())

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        for key, expected in (('a', 'b'), ('c', ('b', 'd')), ('z', 'd')):
            cursor.set_key(key)
            self.assertNotEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND,
                f'search_near must find a neighbor in ingest for {key}')
            if isinstance(expected, tuple):
                self.assertIn(cursor.get_key(), expected)
            else:
                self.assertEqual(cursor.get_key(), expected)
        self.session.rollback_transaction()
        cursor.close()

        # With both constituents empty there is nothing to position on.
        cursor = self.session.open_cursor(empty_uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        cursor.set_key('a')
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

    # largest_key ignores visibility, so it reports a key from a transaction that has not committed.
    def test_largest_key_with_uncommitted_write(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's'}, 10)

        self.set_step_down_ts(20)

        def largest():
            c = self.session.open_cursor(self.uri, None, None)
            self.assertEqual(c.largest_key(), 0)
            key = c.get_key()
            c.close()
            return key

        self.assertEqual(largest(), 'd')

        # A second session inserts a new maximum into ingest and holds the transaction open.
        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(self.uri, None, None)
        wsession.begin_transaction()
        wcur['zz'] = 'uncommitted'

        self.assertEqual(largest(), 'zz',
            'largest_key must report the uncommitted key it cannot read')

        # The aborted update stays in the tree until reconciliation discards it, and largest_key
        # consults no visibility state, so the abandoned key may still be the reported maximum.
        wsession.rollback_transaction()
        wcur.close()
        wsession.close()
        self.assertIn(largest(), ('d', 'zz'))

    # A reverse walk interrupted by the cutoff re-seats the same way a forward walk does.
    def test_reverse_iteration_across_step_down_ts(self):
        uri = f'layered:{self.test_name}_reviter'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        stable_keys = [f'k{i:02d}' for i in range(0, 20, 2)]
        self.write_at(uri, {k: 'v' for k in stable_keys}, 10)

        wsession = self.conn.open_session()
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))

        # Walk part way backwards, then set the cutoff mid-iteration.
        seen = []
        for _ in range(4):
            self.assertEqual(cursor.prev(), 0)
            seen.append(cursor.get_key())
        self.set_step_down_ts(50)

        # Interleave ingest keys both behind and ahead of the scan position, and update and remove
        # stable keys the backward walk has not reached yet.
        updated = stable_keys[3]
        removed = stable_keys[1]
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

        kvs = []
        while cursor.prev() == 0:
            seen.append(cursor.get_key())
            kvs.append((cursor.get_key(), cursor.get_value()))
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(seen, list(reversed(stable_keys)),
            'the reverse walk must yield exactly the snapshot keys once, in order')
        self.assertIn((updated, 'v'), kvs, 'the invisible update must not reach this snapshot')
        self.assertIn((removed, 'v'), kvs, 'the invisible tombstone must not reach this snapshot')

    # A layered tree never opens by checkpoint, before or after the demotion. Reading the step-down
    # checkpoint means opening a checkpoint cursor on the stable constituent, which holds the stable
    # content only: the ingest half was never checkpointed.
    def test_checkpoint_cursor_after_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's'}, 10)

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'a': 'i', 'z': 'i'}, 30)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri, None, 'checkpoint=WiredTigerCheckpoint'),
            '/do not support opening by checkpoint/')

        self.complete_step_down(20)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri, None, 'checkpoint=WiredTigerCheckpoint'),
            '/do not support opening by checkpoint/')

        cursor = self.session.open_cursor(self.stable_uri(self.uri), None,
            'checkpoint=WiredTigerCheckpoint')
        seen = {}
        while cursor.next() == 0:
            seen[cursor.get_key()] = cursor.get_value()
        cursor.close()
        self.assertEqual(seen, {'b': 's', 'd': 's'},
            'the step-down checkpoint must hold exactly the stable content')

# Every kind of write a straddler can attempt after the cutoff is set. Each takes (cursor, key); the
# guard surfaces as an exception, so the return values are unused.
def _op_insert(cursor, key):
    cursor[key] = 'straddle'

def _op_update(cursor, key):
    cursor.set_key(key)
    cursor.set_value('straddle')
    return cursor.update()

def _op_remove(cursor, key):
    cursor.set_key(key)
    return cursor.remove()

def _op_modify(cursor, key):
    cursor.set_key(key)
    return cursor.modify([wiredtiger.Modify('Z', 0, 1)])

def _op_reserve(cursor, key):
    cursor.set_key(key)
    return cursor.reserve()

_straddler_ops = [
    ('insert', dict(do_op=_op_insert)),
    ('update', dict(do_op=_op_update)),
    ('remove', dict(do_op=_op_remove)),
    ('modify', dict(do_op=_op_modify)),
    ('reserve', dict(do_op=_op_reserve)),
]

# The straddler guard fires on every kind of write, not just the insert and remove spelled out in
# test_layered_async_stepdown03.py. This lives in its own class so the operation axis does not
# multiply the tests above.
@disagg_test_class
class test_layered_async_stepdown07_straddler_ops(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, _straddler_ops)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # A transaction that began before the cutoff was set rolls back on its first write, whichever
    # write it is, and leaves nothing behind in either constituent.
    def test_straddler_write_rolls_back(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'base'}, 10)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()

        self.set_step_down_ts(20)

        self.assert_step_down_rollback(lambda: self.do_op(cursor, 'k1'))
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'base'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), set())
        self.assertEqual(self.read_kvs_at(self.stable_uri(self.uri), 40), {'k1': 'base'})

# Write-conflict detection around the cutoff and the demotion, plus a checkpoint taken while the
# cutoff is set.
@disagg_test_class
class test_layered_async_stepdown07_write_conflicts(LayeredStepdownMixin,
                                                   wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # Two writers that both began after the cutoff was set collide on the same ingest key.
    def test_conflict_both_after_cutoff(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'stable'}, 10)

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'first'

        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(self.uri, None, None)
        wsession.begin_transaction()
        wcur.set_key('k1')
        wcur.set_value('second')
        self.expect_conflict_rollback(wcur.update)
        wsession.rollback_transaction()
        wcur.close()
        wsession.close()

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'first'})

    # An uncommitted ingest write survives the demotion; a follower writer must still see it.
    def test_conflict_across_demotion(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'stable'}, 10)

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'held'

        # The step-down completes with the write still uncommitted.
        self.complete_step_down(20)

        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(self.uri, None, None)
        wsession.begin_transaction()
        wcur.set_key('k1')
        wcur.set_value('follower')
        self.expect_conflict_rollback(wcur.update)
        wsession.rollback_transaction()
        wcur.close()
        wsession.close()

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'held'})

    # A writer reading below the cutoff must still collide with a committed stable update that is
    # newer than its read timestamp: the update it would overwrite is one it cannot see.
    def test_conflict_read_ts_below_stable_update(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'old'}, 10)
        self.write_at(self.uri, {'k1': 'newer'}, 15)

        self.set_step_down_ts(20)

        # The transaction begins after the cutoff, so it is not a straddler, but it reads below the
        # stable update at 15.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(12))
        self.assertEqual(cursor['k1'], 'old')
        cursor.set_key('k1')
        cursor.set_value('doomed')
        self.expect_conflict_rollback(cursor.update)
        self.session.rollback_transaction()

        # The rejected write left both constituents alone.
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'newer'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), set())

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(12))
        cursor['other'] = 'fine'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'newer', 'other': 'fine'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'other'})

    # A checkpoint taken while the cutoff is set, before stable reaches it, changes nothing for
    # readers or for routing.
    def test_extra_checkpoint_while_cutoff_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's'}, 10)

        self.set_step_down_ts(30)
        self.write_at(self.uri, {'a': 'i', 'z': 'i'}, 40)

        before = self.read_kvs_at(self.uri, 50)

        # Advance stable part way to the cutoff first, so the extra checkpoint actually persists the
        # stable content instead of being an empty checkpoint that proves nothing.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()

        # The checkpoint holds the stable half and nothing else.
        ckpt_cursor = self.session.open_cursor(self.stable_uri(self.uri), None,
            'checkpoint=WiredTigerCheckpoint')
        checkpointed = {}
        while ckpt_cursor.next() == 0:
            checkpointed[ckpt_cursor.get_key()] = ckpt_cursor.get_value()
        ckpt_cursor.close()
        self.assertEqual(checkpointed, {'b': 's', 'd': 's'})

        self.assertEqual(self.read_kvs_at(self.uri, 50), before)
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 50), {'b', 'd'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 50), {'a', 'z'})

        # A write after that checkpoint still routes to ingest, and the step-down still completes.
        self.write_at(self.uri, {'y': 'i'}, 45)
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 50), {'a', 'z', 'y'})
        self.complete_step_down(30)
        self.assertEqual(self.read_kvs_at(self.uri, 50),
            {'b': 's', 'd': 's', 'a': 'i', 'z': 'i', 'y': 'i'})
