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

# test_layered_txn02.py
#   Rolling back an ordinary, non-prepared write to a layered table. The rolled-back update must
#   leave no trace: the key keeps whatever the last committed transaction gave it, and a key the
#   rolled-back transaction created stays absent.
#
#   Each test runs against a key seeded into the ingest constituent, the stable constituent, or
#   both (the 'place' scenario), since the rollback has to reach whichever constituent the write
#   landed in. Writes and reads both go through the follower, where the two constituents coexist.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_txn02(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    # Which constituent(s) hold the key before the transaction under test.
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
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, 'key_format=S,value_format=S')

    # Write on the follower, so the items live in the ingest constituent.
    def write_to_ingest(self, items):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        for key, value in items.items():
            cursor[key] = value
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()

    # Write on the leader and pull the result into the follower through a checkpoint, so the items
    # live in the stable constituent.
    def write_to_stable(self, items):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for key, value in items.items():
            cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)

    # Seed the items into the constituent(s) the scenario selects.
    def seed(self, items):
        if self.place in ('stable', 'both'):
            self.write_to_stable(items)
        if self.place in ('ingest', 'both'):
            self.write_to_ingest(items)

    # The value the follower sees for a key, or None if it is absent.
    def read_value(self, key):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        cursor.set_key(key)
        value = cursor.get_value() if cursor.search() == 0 else None
        self.session_follow.rollback_transaction()
        cursor.close()
        return value

    # The keys the follower can reach in order, so a rolled-back write cannot hide from a scan.
    def scan(self):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        keys = []
        while cursor.next() == 0:
            keys.append(cursor.get_key())
        self.session_follow.rollback_transaction()
        cursor.close()
        return keys

    # Run ops against a follower cursor and roll the transaction back.
    def rollback(self, ops):
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        ops(cursor)
        self.session_follow.rollback_transaction()
        cursor.close()

    # An update rolled back leaves the previously committed value in place.
    def test_rollback_update(self):
        self.seed({'k': 'seeded'})
        self.rollback(lambda cursor: cursor.__setitem__('k', 'discarded'))
        self.assertEqual(self.read_value('k'), 'seeded')
        self.assertEqual(self.scan(), ['k'])

    # An insert of a brand new key rolled back leaves the key absent.
    def test_rollback_insert(self):
        self.seed({'k': 'seeded'})
        self.rollback(lambda cursor: cursor.__setitem__('new', 'discarded'))
        self.assertEqual(self.read_value('new'), None)
        self.assertEqual(self.scan(), ['k'])

    # A remove rolled back leaves the key readable.
    def test_rollback_remove(self):
        self.seed({'k': 'seeded'})
        def remove(cursor):
            cursor.set_key('k')
            self.assertEqual(cursor.remove(), 0)
        self.rollback(remove)
        self.assertEqual(self.read_value('k'), 'seeded')
        self.assertEqual(self.scan(), ['k'])

    # Several writes to the same key in one rolled-back transaction all disappear together.
    def test_rollback_repeated_update(self):
        self.seed({'k': 'seeded'})
        def update_thrice(cursor):
            for value in ('one', 'two', 'three'):
                cursor['k'] = value
        self.rollback(update_thrice)
        self.assertEqual(self.read_value('k'), 'seeded')

    # A rollback does not disturb keys the transaction did not touch.
    def test_rollback_leaves_other_keys(self):
        self.seed({'k': 'seeded', 'other': 'untouched'})
        self.rollback(lambda cursor: cursor.__setitem__('k', 'discarded'))
        self.assertEqual(self.read_value('k'), 'seeded')
        self.assertEqual(self.read_value('other'), 'untouched')
        self.assertEqual(self.scan(), ['k', 'other'])

    # A committed write after a rolled-back one is unaffected by the rollback.
    def test_commit_after_rollback(self):
        self.seed({'k': 'seeded'})
        self.rollback(lambda cursor: cursor.__setitem__('k', 'discarded'))
        self.assertEqual(self.read_value('k'), 'seeded')
        cursor = self.session_follow.open_cursor(self.uri)
        self.session_follow.begin_transaction()
        cursor['k'] = 'committed'
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.assertEqual(self.read_value('k'), 'committed')

if __name__ == '__main__':
    wttest.run()
