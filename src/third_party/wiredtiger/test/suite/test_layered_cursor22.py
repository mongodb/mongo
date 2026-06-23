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

# test_layered_cursor22.py
#   Positional layered cursor operations on a disaggregated follower that span a transaction
#   boundary. Releasing the snapshot at the commit/begin localizes the cursor's value, so an
#   operation that runs in a later transaction than its positioning search must re-read the record
#   rather than trust the cached value. A positional remove used to misbehave here (it stacked a
#   second tombstone on an already-deleted key).
#
#   Every test runs against a key held in the ingest constituent, the stable constituent, or both
#   (the 'place' scenario). Placement is the only thing that differs between the cases; the
#   positional operation and its expected result are identical in all three.

import wiredtiger, wttest
from wiredtiger import stat, WT_NOTFOUND
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_cursor22(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    # Which constituent(s) hold the key under test.
    placement = [
        ('ingest', dict(place='ingest')),
        ('stable', dict(place='stable')),
        ('both',   dict(place='both')),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, placement)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, 'key_format=i,value_format=S')

    # Write the items {key: value} on the follower, so they live in the ingest constituent.
    def write_to_ingest(self, items):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        for key, value in items.items():
            cursor[key] = value
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()

    # Write the items on the leader and pull them into the follower via a checkpoint, so they live
    # in the stable constituent.
    def write_to_stable(self, items):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for key, value in items.items():
            cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    # Seed the items into the constituent(s) selected by the scenario.
    def seed(self, items):
        if self.place in ('stable', 'both'):
            self.write_to_stable(items)
        if self.place in ('ingest', 'both'):
            self.write_to_ingest(items)

    # Open a follower cursor positioned on a key with an auto-commit search; the search commits, so
    # any operation that follows runs in a separate, later transaction working from a localized
    # value.
    def position_follower(self, key):
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        return cursor

    # The value the follower sees for a key, or None if it is gone, read through a fresh cursor.
    def read_value(self, key):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        cursor.set_key(key)
        value = cursor.get_value() if cursor.search() == 0 else None
        self.session_follow.rollback_transaction()
        cursor.close()
        return value

    # The running count of layered cursor remove operations.
    def get_current_remove_number(self):
        stat_cursor = self.session_follow.open_cursor('statistics:')
        number = stat_cursor[stat.conn.layered_curs_remove][2]
        stat_cursor.close()
        return number

    # A positional remove issued in a later transaction removes the key and stays positioned on it.
    def test_remove(self):
        self.seed({1: 'v'})
        cursor = self.position_follower(1)

        self.session_follow.begin_transaction()
        self.assertEqual(cursor.remove(), 0)
        self.assertEqual(cursor.get_key(), 1)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.assertEqual(self.read_value(1), None)

    # A key deleted before the later-transaction remove must return WT_NOTFOUND without writing a
    # second, consecutive tombstone.
    def test_remove_already_deleted(self):
        self.seed({1: 'v'})
        cursor = self.position_follower(1)

        # Delete the key with a second cursor before the positional remove runs.
        delete_cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        delete_cursor.set_key(1)
        self.assertEqual(delete_cursor.remove(), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        delete_cursor.close()

        # The positional remove finds the tombstone, returns WT_NOTFOUND, and writes nothing more.
        before = self.get_current_remove_number()
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        self.assertEqual(cursor.remove(), WT_NOTFOUND)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(11))
        self.assertEqual(self.get_current_remove_number(), before)

    # A positional update issued in a later transaction writes the new value.
    def test_update(self):
        self.seed({1: 'v'})
        cursor = self.position_follower(1)

        self.session_follow.begin_transaction()
        cursor.set_value('w')
        self.assertEqual(cursor.update(), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.assertEqual(self.read_value(1), 'w')

    # A positional modify issued in a later transaction builds on the current value.
    def test_modify(self):
        self.seed({1: 'v'})
        cursor = self.position_follower(1)

        self.session_follow.begin_transaction()
        self.assertEqual(cursor.modify([wiredtiger.Modify('X', 0, 1)]), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.assertEqual(self.read_value(1), 'X')

    # A positional reserve only makes sense as a precursor to a change, so reserve then update in
    # the same later transaction and confirm the new value is written.
    def test_reserve(self):
        self.seed({1: 'v'})
        cursor = self.position_follower(1)

        self.session_follow.begin_transaction()
        self.assertEqual(cursor.reserve(), 0)
        cursor.set_value('w')
        self.assertEqual(cursor.update(), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.assertEqual(self.read_value(1), 'w')

    # Run one next()/prev() in its own transaction (so the iteration crosses a boundary).
    def iterate_step(self, cursor, step):
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(5))
        ret = step()
        result = cursor.get_key() if ret == 0 else ret
        self.session_follow.commit_transaction()
        return result

    # next/prev iteration, including direction changes (zigzag), continues correctly across
    # transaction boundaries.
    def test_iterate(self):
        self.seed({1: 'v', 2: 'v', 3: 'v'})
        cursor = self.session_follow.open_cursor(self.uri)

        # Each step crosses a transaction boundary: forward, forward, reverse, then zigzag forward.
        self.assertEqual(self.iterate_step(cursor, cursor.next), 1)
        self.assertEqual(self.iterate_step(cursor, cursor.next), 2)
        self.assertEqual(self.iterate_step(cursor, cursor.prev), 1)
        self.assertEqual(self.iterate_step(cursor, cursor.next), 2)
        self.assertEqual(self.iterate_step(cursor, cursor.next), 3)
        self.assertEqual(self.iterate_step(cursor, cursor.next), WT_NOTFOUND)
        self.assertEqual(self.iterate_step(cursor, cursor.prev), 3)
        self.assertEqual(self.iterate_step(cursor, cursor.prev), 2)
        self.assertEqual(self.iterate_step(cursor, cursor.prev), 1)
        self.assertEqual(self.iterate_step(cursor, cursor.prev), WT_NOTFOUND)
