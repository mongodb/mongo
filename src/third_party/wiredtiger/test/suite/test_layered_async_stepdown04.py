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
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# test_layered_async_stepdown04.py
#    Operational surfaces: schema ops, cached-cursor reuse and cursor configurations.
@disagg_test_class
class test_layered_async_stepdown04(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    test_name = __qualname__

    uri = f'layered:{test_name}'

    # The connection-wide count of cursors reused from the session cursor cache.
    def cursor_reopen_count(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        count = stat_cursor[stat.conn.cursor_reopen][2]
        stat_cursor.close()
        return count

    # Open a cursor and assert the layered handle came from the session cursor cache.
    def open_cached_cursor(self, uri):
        before = self.cursor_reopen_count()
        cursor = self.session.open_cursor(uri, None, None)
        self.assertEqual(self.cursor_reopen_count() - before, 1,
            'the layered cursor must be served from the session cursor cache')
        return cursor

    # A table created while the timestamp is set routes its writes to ingest, leaves stable empty,
    # and survives the demotion.
    def test_create_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.set_step_down_ts(20)

        uri = f'layered:{self.test_name}_create'
        self.session.create(uri, 'key_format=S,value_format=S')
        self.write_at(uri, {'k1': 'v', 'k2': 'v'}, 30)

        self.assertEqual(self.read_keys_at(self.ingest_uri(uri), 40), {'k1', 'k2'})
        self.assertEqual(self.read_keys_at(self.stable_uri(uri), 40), set())
        self.assertEqual(self.read_keys_at(uri, 40), {'k1', 'k2'})

        # The table has no stable content at all, so the demotion is its first checkpoint.
        self.complete_step_down(20)
        self.assertEqual(self.read_kvs_at(uri, 40), {'k1': 'v', 'k2': 'v'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(uri), 40), {'k1', 'k2'})

        # A follower write reaches the same table.
        self.write_at(uri, {'k3': 'v'}, 50)
        self.assertEqual(self.read_keys_at(uri, 60), {'k1', 'k2', 'k3'})

    # A table with stable content can be dropped while the timestamp is set.
    def test_drop_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'v'}, 10)

        self.set_step_down_ts(20)
        self.dropUntilSuccess(self.session, self.uri)

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri, None, None))

        # The drop is not undone by the demotion.
        self.complete_step_down(20)
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.uri, None, None))

    # A cursor reused from the cache picks up the new routing: its writes go to ingest.
    def test_cached_cursor_reuse_across_step_down_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['k1'] = 'stable'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()

        self.set_step_down_ts(20)

        cursor = self.open_cached_cursor(self.uri)

        self.session.begin_transaction()
        cursor['k2'] = 'ingest'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'k2'})
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), {'k1'})
        self.assertEqual(self.read_keys_at(self.uri, 40), {'k1', 'k2'})

    # A cursor closed before the demotion and reopened afterwards serves the surviving content.
    def test_cached_cursor_reuse_across_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'pre': 'stable'}, 10)

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'post': 'ingest'}, 30)

        self.complete_step_down(20)

        cursor = self.open_cached_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        seen = set()
        while cursor.next() == 0:
            seen.add(cursor.get_key())
        self.session.rollback_transaction()
        cursor.close()
        self.assertEqual(seen, {'pre', 'post'},
            'all content must be readable through a reopened cursor after the step-down')

        # A write through the cache-served cursor after the demotion still routes to ingest.
        cursor = self.open_cached_cursor(self.uri)
        self.session.begin_transaction()
        cursor['follower'] = 'ingest'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        cursor.close()

        self.assertIn('follower', self.read_keys_at(self.ingest_uri(self.uri), 60))
        self.assertEqual(self.read_keys_at(self.uri, 60), {'pre', 'post', 'follower'})

    # Bounds set before the timestamp apply to keys from both constituents.
    def test_bounded_cursor_across_step_down_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'b': 's', 'd': 's', 'f': 's'}, 10)

        cursor = self.session.open_cursor(self.uri, None, None)
        cursor.set_key('b')
        self.assertEqual(cursor.bound('action=set,bound=lower'), 0)
        cursor.set_key('e')
        self.assertEqual(cursor.bound('action=set,bound=upper'), 0)

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'a': 'i', 'c': 'i', 'e': 'i'}, 30)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        seen = []
        while cursor.next() == 0:
            seen.append(cursor.get_key())
        self.session.rollback_transaction()

        self.assertEqual(seen, ['b', 'c', 'd', 'e'],
            'a bounded scan must respect its bounds on both constituents')

        # Walking out of the bounds resets the cursor, which clears them, so the same bounds are
        # applied again for the post-demotion walk.
        self.complete_step_down(20)
        cursor.set_key('b')
        self.assertEqual(cursor.bound('action=set,bound=lower'), 0)
        cursor.set_key('e')
        self.assertEqual(cursor.bound('action=set,bound=upper'), 0)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        seen = []
        while cursor.next() == 0:
            seen.append(cursor.get_key())
        self.session.rollback_transaction()

        self.assertEqual(seen, ['b', 'c', 'd', 'e'],
            'the bounds must still clamp the merged view after the demotion')

        # Follower writes land in ingest, one inside the bounds and one past the upper bound.
        self.write_at(self.uri, {'cc': 'f', 'z': 'f'}, 50)

        cursor.set_key('b')
        self.assertEqual(cursor.bound('action=set,bound=lower'), 0)
        cursor.set_key('e')
        self.assertEqual(cursor.bound('action=set,bound=upper'), 0)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        seen = []
        while cursor.next() == 0:
            seen.append(cursor.get_key())
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(seen, ['b', 'c', 'cc', 'd', 'e'],
            'the bounds must clamp content written after the demotion')

    # A readonly cursor reads the merged view and still rejects writes.
    def test_readonly_cursor_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'stable'}, 10)

        self.set_step_down_ts(20)
        self.write_at(self.uri, {'k2': 'ingest'}, 30)

        cursor = self.session.open_cursor(self.uri, None, 'readonly=true')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        self.assertEqual(cursor['k1'], 'stable')
        self.assertEqual(cursor['k2'], 'ingest')
        self.session.rollback_transaction()

        self.session.begin_transaction()
        cursor.set_key('k3')
        cursor.set_value('v')
        with self.expectedStderrPattern('Unsupported cursor operation'):
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.insert())
        self.session.rollback_transaction()

        self.complete_step_down(20)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        self.assertEqual(cursor['k1'], 'stable')
        self.assertEqual(cursor['k2'], 'ingest')
        self.session.rollback_transaction()

        self.session.begin_transaction()
        cursor.set_key('k3')
        cursor.set_value('v')
        with self.expectedStderrPattern('Unsupported cursor operation'):
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.insert())
        self.session.rollback_transaction()
        cursor.close()

    # A sample only ever comes from the visible merged view: never a removed key, and never a key
    # from outside the table. Which constituent a sample is drawn from is not part of the contract,
    # so the observed split is reported rather than asserted.
    def test_next_random_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        stable_keys = {f's{i:02d}' for i in range(10)}
        self.write_at(self.uri, {k: 's' for k in stable_keys}, 10)

        self.set_step_down_ts(20)

        ingest_keys = {f'i{i:02d}' for i in range(10)}
        self.write_at(self.uri, {k: 'i' for k in ingest_keys}, 30)

        # Remove one key from each constituent; a tombstone in ingest hides the stable key.
        removed = {'s00', 'i00'}
        self.remove_at(self.uri, sorted(removed), 40)

        visible = (stable_keys | ingest_keys) - removed
        cursor = self.session.open_cursor(self.uri, None, 'next_random=true')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        sampled = set()
        for _ in range(100):
            self.assertEqual(cursor.next(), 0)
            key = cursor.get_key()
            self.assertIn(key, visible, 'a sample must come from the visible merged view')
            sampled.add(key)
        self.session.rollback_transaction()
        cursor.close()

        self.pr(f'next_random sampled {len(sampled & stable_keys)} stable and '
                f'{len(sampled & ingest_keys)} ingest keys over 100 draws')

        # Sampling 100 keys out of 18 only probably lands on a removed one, so narrow the table to a
        # single survivor: now every draw must return it, whichever constituent it came from.
        survivor = 's01'
        self.remove_at(self.uri, sorted(visible - {survivor}), 50)

        cursor = self.session.open_cursor(self.uri, None, 'next_random=true')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(60))
        for _ in range(20):
            self.assertEqual(cursor.next(), 0)
            self.assertEqual(cursor.get_key(), survivor,
                'the only visible key must be the only key sampled')
        self.session.rollback_transaction()
        cursor.close()

        # With that one gone too there is nothing left to sample.
        self.remove_at(self.uri, [survivor], 70)
        cursor = self.session.open_cursor(self.uri, None, 'next_random=true')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(80))
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

    # The demotion happens with the table still holding an ingest/stable mix, so sampling afterwards
    # is bound by the same contract: only visible merged keys come back.
    def test_next_random_after_step_down(self):
        uri = f'layered:{self.test_name}_random_after'
        self.set_global_ts(1, 1)
        self.session.create(uri, 'key_format=S,value_format=S')

        stable_keys = {f's{i:02d}' for i in range(10)}
        self.write_at(uri, {k: 's' for k in stable_keys}, 10)

        self.set_step_down_ts(20)

        ingest_keys = {f'i{i:02d}' for i in range(10)}
        self.write_at(uri, {k: 'i' for k in ingest_keys}, 30)

        removed = {'s00', 'i00'}
        self.remove_at(uri, sorted(removed), 40)

        visible = (stable_keys | ingest_keys) - removed
        self.complete_step_down(20)
        self.assertEqual(self.read_keys_at(uri, 50), visible,
            'the merged view must be unchanged by the demotion')

        cursor = self.session.open_cursor(uri, None, 'next_random=true')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        for _ in range(100):
            self.assertEqual(cursor.next(), 0)
            self.assertIn(cursor.get_key(), visible,
                'a sample after the demotion must come from the visible merged view')
        self.session.rollback_transaction()
        cursor.close()
