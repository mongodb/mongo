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

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_hs19.py
# Ensure eviction doesn't clear the history store again after checkpoint has done so because of the same update without timestamp.
class test_hs19(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=5MB,eviction=(threads_max=1)'
    key_format_values = [
        ('column', dict(key_format='r')),
        ('string-row', dict(key_format='S'))
    ]
    scenarios = make_scenarios(key_format_values)

    def create_key(self, i):
        if self.key_format == 'S':
            return str(i)
        return i

    def test_hs19(self):
        uri = 'table:test_hs19'
        junk_uri = 'table:junk'
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        session2 = self.conn.open_session()
        session2.create(junk_uri, 'key_format={},value_format=S'.format(self.key_format))
        cursor2 = session2.open_cursor(junk_uri)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        value1 = 'a' * 500
        value2 = 'b' * 500
        value3 = 'c' * 50000

        # Insert an update without timestamp.
        self.session.begin_transaction()
        cursor[self.create_key(1)] = value1
        self.session.commit_transaction()

        # Do 2 modifies.
        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        mods = [wiredtiger.Modify('B', 100, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        mods = [wiredtiger.Modify('C', 101, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Start a transaction to pin back the reconciliation last running value.
        session2.begin_transaction()
        cursor2[self.create_key(1)] = value3

        # Insert a modify ahead of our reconstructed modify, this one will be used unintentionally
        # to reconstruct the final value, corrupting the resulting value.
        # The 0 at the end of the modify call indicates how many bytes to replace, we keep
        # it as 0 here to not overwrite any of the existing value.
        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        mods = [wiredtiger.Modify('AAAAAAAAAA', 102, 0)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        # Insert a modify to get written as the on disk value by checkpoint.
        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        mods = [wiredtiger.Modify('D', 102, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # Checkpoint such that all modifies get written out to the history store and the latest
        # modify gets written to the on disk value.
        self.session.checkpoint('use_timestamp=true')

        # Add an additional modify so that when eviction sees this page it will rewrite it as it's
        # dirty.
        self.session.begin_transaction()
        cursor.set_key(self.create_key(1))
        mods = [wiredtiger.Modify('E', 103, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(6))

        # First deposition the first cursor, so the page can be evicted.
        cursor.reset()
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        # Search for the key so we position our cursor on the page that we want to evict.
        evict_cursor.set_key(self.create_key(1))
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()

        # Construct and test the value as at timestamp 2
        expected = list(value1)
        expected[100] = 'B'
        expected = str().join(expected)

        # Retrieve the value at timestamp 2.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(2))
        cursor.set_key(self.create_key(1))
        cursor.search()

        # Assert that it matches our expected value.
        self.assertEqual(cursor.get_value(), expected)
        self.session.rollback_transaction()

        # Construct and test the value as at timestamp 2
        expected = list(expected)
        expected[101] = 'C'
        expected = str().join(expected)

        # Retrieve the value at timestamp 3.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(3))
        cursor.set_key(self.create_key(1))
        cursor.search()

        # Assert that it matches our expected value.
        self.assertEqual(cursor.get_value(), expected)
        self.session.rollback_transaction()

        # Construct and test the value as at timestamp 4
        expected = list(expected)
        for x in range(10):
            expected[102 + x] = 'A'
            expected.append('a')
        expected = str().join(expected)

        # Retrieve the value at timestamp 1.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(4))
        cursor.set_key(self.create_key(1))
        cursor.search()
        # Assert that it matches our expected value.
        self.assertEqual(cursor.get_value(), expected)
        self.session.rollback_transaction()
