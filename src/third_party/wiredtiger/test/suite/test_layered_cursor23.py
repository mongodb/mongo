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
from wtscenario import make_scenarios

# When a read timestamp is set, a layered cursor must behave exactly like a classic cursor for the
# same logical key state. The expected outcome is determined live: each scenario loads a regular
# (non-layered) reference table holding the values the role's cursor sees, and the same operations
# run against both.
#
# The four scenario flags place a visible (commit_ts < read_ts) and/or an invisible
# (read_ts < commit_ts) committed version on the stable and/or ingest constituent. The reference
# table is loaded with what the role's layered cursor sees:
#   leader role  : stable values only (a leader disregards the ingest table);
#   follower role: the union of stable and ingest (a value present in either constituent).
#
# test_methods drives every write method on a single key and compares the outcomes; the prime
# flag first positions the cursor on a different key, so the write-conflict check must not trust
# that stale constituent position.
# test_traverse brackets the scenario key with two neighbors (one visible only on ingest, one
# only on stable) and walks the table with next()/prev(), optionally applying a write to the
# current record between steps, comparing the whole sequence to the classic cursor. The neighbors
# ensure a mid-walk write on a follower resets the alternate constituent while the other still has
# keys to return -- which must not drop them from the scan.
@disagg_test_class
class test_layered_cursor23(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    uri_ref = f'table:{test_name}_ref'
    key = 'k'
    prime_key = 'j'
    conn_base_config = ',create,statistics=(all),'

    # Visible commit < read < invisible commit <= checkpoint; ingest is newer than stable.
    OLDEST, VIS, READ, INVIS, CKPT = 1, 100, 150, 200, 250
    VIS_INGEST, INVIS_INGEST = 110, 210

    roles = [('leader', dict(role='leader')), ('follower', dict(role='follower'))]
    overwrite_opts = [('ow_true', dict(overwrite=True)), ('ow_false', dict(overwrite=False))]
    vis_stable_opts = [('vs1', dict(vis_stable=True)), ('vs0', dict(vis_stable=False))]
    vis_ingest_opts = [('vi1', dict(vis_ingest=True)), ('vi0', dict(vis_ingest=False))]
    invis_stable_opts = [('xs1', dict(invis_stable=True)), ('xs0', dict(invis_stable=False))]
    invis_ingest_opts = [('xi1', dict(invis_ingest=True)), ('xi0', dict(invis_ingest=False))]
    prime_opts = [('no_prime', dict(prime=None)), ('prime_stable', dict(prime='stable')),
        ('prime_ingest', dict(prime='ingest'))]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, roles, overwrite_opts,
        vis_stable_opts, vis_ingest_opts, invis_stable_opts, invis_ingest_opts, prime_opts)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.test_session = self.session if self.role == 'leader' else self.session_follow

    # --- shared setup helpers ---

    def commit(self, session, uri, key, value, ts):
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        cursor[key] = value
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def build(self, session, uri, key, visible, invisible, vis_ts, invis_ts):
        if visible:
            self.commit(session, uri, key, 'vis', vis_ts)
        if invisible:
            self.commit(session, uri, key, 'invis', invis_ts)

    # The (visible, invisible) state the role's cursor sees for a key in the given constituent state.
    def ref_state(self, vis_stable, vis_ingest, invis_stable, invis_ingest):
        if self.role == 'leader':
            return vis_stable, invis_stable
        return (vis_stable or vis_ingest), (invis_stable or invis_ingest)

    # Load a key's constituent state into the layered table and the matching value into the reference.
    def build_key(self, key, vis_stable, vis_ingest, invis_stable, invis_ingest):
        ref_visible, ref_invisible = self.ref_state(vis_stable, vis_ingest, invis_stable, invis_ingest)
        self.build(self.session, self.uri_ref, key, ref_visible, ref_invisible, self.VIS, self.INVIS)
        self.build(self.session, self.uri, key, vis_stable, invis_stable, self.VIS, self.INVIS)
        self.build(self.session_follow, self.uri, key, vis_ingest, invis_ingest,
            self.VIS_INGEST, self.INVIS_INGEST)

    def create_tables(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')
        self.session.create(self.uri_ref, 'key_format=S,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.OLDEST))
        self.conn_follow.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.OLDEST))

    def checkpoint_and_advance(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.CKPT))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    # --- test_methods: every write method on a single key ---

    _ops = {
        'insert': lambda c: c.insert(),
        'update': lambda c: c.update(),
        'modify': lambda c: c.modify([wiredtiger.Modify('X', 0, 1)]),
        'reserve': lambda c: c.reserve(),
        'remove': lambda c: c.remove(),
    }

    # Run a write method at the read timestamp on the given table; normalize the result, then roll
    # back so the prepared state is unchanged for the next method.
    def run_method(self, session, uri, method):
        cursor = session.open_cursor(uri, None, f'overwrite={str(self.overwrite).lower()}')
        session.begin_transaction('read_timestamp=' + self.timestamp_str(self.READ))
        if self.prime is not None:
            # Position cursor (stable or ingest) on a different key first.
            cursor.set_key(self.prime_key)
            self.assertEqual(cursor.search(), 0)
        cursor.set_key(self.key)
        if method in ('insert', 'update'):
            cursor.set_value('new')
        try:
            ret = self._ops[method](cursor)
            outcome = 'WT_NOTFOUND' if ret == wiredtiger.WT_NOTFOUND else 'success'
        except wiredtiger.WiredTigerRollbackError:
            outcome = 'WT_ROLLBACK'
        except wiredtiger.WiredTigerError as e:
            s = str(e)
            if 'WT_DUPLICATE_KEY' in s:
                outcome = 'WT_DUPLICATE_KEY'
            elif 'WT_NOTFOUND' in s:
                outcome = 'WT_NOTFOUND'
            else:
                outcome = f'ERROR({e})'
        finally:
            session.rollback_transaction()
            cursor.close()
        return outcome

    def test_methods(self):
        if self.prime == 'ingest' and self.role == 'leader':
            self.skipTest('prime_ingest is not meaningful for the leader role')
        self.create_tables()
        self.build_key(self.key, self.vis_stable, self.vis_ingest,
            self.invis_stable, self.invis_ingest)
        if self.prime is not None:
            self.build_key(
                self.prime_key, self.prime == 'stable', self.prime == 'ingest', False, False)
        self.checkpoint_and_advance()

        mismatches = []
        for method in ('insert', 'update', 'modify', 'reserve', 'remove'):
            expected = self.run_method(self.session, self.uri_ref, method)
            actual = self.run_method(self.test_session, self.uri, method)
            if actual != expected:
                mismatches.append(f'{method}: layered={actual} ref={expected}')
        self.assertEqual(mismatches, [],
            f'role={self.role} ow={self.overwrite} '
            f'vis(s={self.vis_stable},i={self.vis_ingest}) '
            f'invis(s={self.invis_stable},i={self.invis_ingest}): {"; ".join(mismatches)}')

    # --- test_traverse: walk the table, optionally writing the current record between steps ---

    # Apply a write method to the currently positioned record. Returns a normalized outcome.
    def apply_positioned(self, cursor, method):
        if method == 'update':
            cursor.set_value('new')
            ret = cursor.update()
        elif method == 'remove':
            ret = cursor.remove()
        else:
            ret = cursor.modify([wiredtiger.Modify('Y', 0, 1)])
        return 'WT_NOTFOUND' if ret == wiredtiger.WT_NOTFOUND else 'ok'

    # Walk a table at the read timestamp, recording each step. Optionally apply a write method to the
    # current record between steps. Roll back at the end so no change persists.
    def observe(self, session, uri, direction, method):
        cursor = session.open_cursor(uri, None, 'overwrite=' + str(self.overwrite).lower())
        session.begin_transaction('read_timestamp=' + self.timestamp_str(self.READ))
        advance = cursor.next if direction == 'next' else cursor.prev
        obs = []
        try:
            while True:
                try:
                    ret = advance()
                except wiredtiger.WiredTigerRollbackError:
                    obs.append(('walk', 'WT_ROLLBACK'))
                    break
                if ret == wiredtiger.WT_NOTFOUND:
                    obs.append(('walk', 'end'))
                    break
                obs.append(('walk', cursor.get_key(), cursor.get_value()))
                if method is None:
                    continue
                try:
                    obs.append((method, self.apply_positioned(cursor, method)))
                except wiredtiger.WiredTigerRollbackError:
                    # A conflict poisons the transaction; the walk cannot continue.
                    obs.append((method, 'WT_ROLLBACK'))
                    break
                except wiredtiger.WiredTigerError as e:
                    obs.append((method, f'ERROR({e})'))
                    break
        finally:
            session.rollback_transaction()
            cursor.close()
        return obs

    def test_traverse(self):
        if self.prime is not None:
            self.skipTest('pre-positioning applies only to test_methods')
        self.create_tables()
        # Bracket the scenario key with neighbors so the walk spans both constituents: 'a' is
        # visible only on ingest, 'z' only on stable, and the middle key takes the scenario's state.
        # On a follower the successful write on the ingest-only neighbor resets the stable cursor
        # mid-walk; the stable-only neighbor 'z' must still be returned by the rest of the scan.
        self.build_key('a', False, True, False, False)
        self.build_key(self.key, self.vis_stable, self.vis_ingest,
            self.invis_stable, self.invis_ingest)
        self.build_key('z', True, False, False, False)
        self.checkpoint_and_advance()

        mismatches = []
        for direction in ('next', 'prev'):
            for method in (None, 'modify', 'update', 'remove'):
                expected = self.observe(self.session, self.uri_ref, direction, method)
                actual = self.observe(self.test_session, self.uri, direction, method)
                if actual != expected:
                    mismatches.append(f'{direction} interleave={method}:\n'
                        f'    layered={actual}\n    ref    ={expected}')
        self.assertEqual(mismatches, [],
            f'role={self.role} ow={self.overwrite} '
            f'vis(s={self.vis_stable},i={self.vis_ingest}) '
            f'invis(s={self.invis_stable},i={self.invis_ingest}):\n' + '\n'.join(mismatches))
