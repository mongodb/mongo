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

import random
import wiredtiger, wttest
from wtscenario import make_scenarios
from wiredtiger import stat

# test_stat10.py
#
# Check the per-table-type btree stats.
#
# For rows:
#    btree_row_empty_values
#    btree_overflow
# For VLCS:
#    btree_column_deleted
#    btree_column_rle
#    btree_overflow
# For FLCS:
#    btree_column_tws
#
# The other types' stats should remain zero.

class test_stat10(wttest.WiredTigerTestCase):
    uri = 'table:test_stat10'
    conn_config = 'statistics=(all)'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='u')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('string_row', dict(key_format='S', value_format='u')),
    ]

    oldest_values = [
        ('15', dict(oldest=15)),
        ('25', dict(oldest=25)),
        ('35', dict(oldest=35)),
    ]

    stable_values = [
        ('15', dict(stable=15)),
        ('25', dict(stable=25)),
        ('35', dict(stable=35)),
    ]

    def keep(name, d):
        return d['oldest'] <= d['stable']

    scenarios = make_scenarios(format_values, oldest_values, stable_values, include=keep)

    # Get a key.
    def make_key(self, i):
        if self.key_format == 'r':
            return i
        key = 'k' + str(i)
        # Make a few keys overflow keys.
        if i % 47 == 0:
            key += 'blablabla' * 1000
        return key

    # Make an invariant value.
    def make_base_value(self):
        if self.value_format == '8t':
            return 170
        return 'abcde' * 100

    # Make a varying value.
    def make_compound_value(self, i):
        if self.value_format == '8t':
            return i
        val = str(i) + 'abcde'
        if i % 61 == 0:
            # Make an overflow value.
            return val * 10000
        elif i % 67 == 0:
            # Use an empty value.
            return ""
        return val * 100

    def evict(self, k):
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        evict_cursor.set_key(k)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def test_tree_stats(self):
        format = "key_format={},value_format={}".format(self.key_format, self.value_format)
        self.session.create(self.uri, format)

        nrows = 50

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        cursor = self.session.open_cursor(self.uri)

        # Add 50 keys with identical values and 50 keys with different values, at time 20.
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[self.make_key(i)] = self.make_base_value()
        for i in range(nrows + 1, nrows * 2 + 1):
            cursor[self.make_key(i)] = self.make_compound_value(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Delete two keys at time 30.
        self.session.begin_transaction()
        cursor.set_key(self.make_key(44))
        self.assertEqual(cursor.remove(), 0)
        cursor.set_key(self.make_key(66))
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        cursor.close()

        # Move oldest and stable up as dictated by the scenario.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.oldest) +
            ',stable_timestamp=' + self.timestamp_str(self.stable))

        # Evict the page(s) so that e.g. VLCS RLE-encoding and FLCS timestamp handling take place.
        for i in range(1, nrows * 2 + 1, nrows // 10):
            self.evict(self.make_key(i))

        # Read the stats.
        statscursor = self.session.open_cursor('statistics:' + self.uri, None, 'statistics=(all)')

        entries = statscursor[stat.dsrc.btree_entries][2]
        row_empty_values = statscursor[stat.dsrc.btree_row_empty_values][2]
        column_deleted = statscursor[stat.dsrc.btree_column_deleted][2]
        column_rle = statscursor[stat.dsrc.btree_column_rle][2]
        column_tws = statscursor[stat.dsrc.btree_column_tws][2]
        overflow = statscursor[stat.dsrc.btree_overflow][2]

        statscursor.close()

        # Validate the stats.

        # Note that the visibility of timestamped changes to the stats is not always
        # very consistent. This behavior is not clearly specified and (AFAIK) not
        # really intended to be; the purpose of checking it here is not to enforce the
        # current behavior but to make sure the behavior doesn't change unexpectedly.
        # I've kept the timestamp tests and the format tests separate to help clarify
        # this.

        # entries: always 100 for FLCS; for RS and VLCS, when oldest passes 30 the two
        # deleted values show up in the count.
        if self.value_format == '8t':
            self.assertEqual(entries, nrows * 2)
        else:
            if self.oldest > 30:
                self.assertEqual(entries, nrows * 2 - 2)
            else:
                self.assertEqual(entries, nrows * 2)

        # row_empty_values: 1 for RS, otherwise 0; only appears when oldest passes 20
        if self.key_format == 'S':
            if self.oldest > 20:
                self.assertEqual(row_empty_values, 1)
            else:
                self.assertEqual(row_empty_values, 0)
        else:
            self.assertEqual(row_empty_values, 0)

        # column_deleted: for VLCS only; only appears when oldest passes 30.
        if self.key_format == 'r' and self.value_format != '8t':
            if self.oldest > 30:
                self.assertEqual(column_deleted, 2)
            else:
                self.assertEqual(column_deleted, 0)
        else:
            self.assertEqual(column_deleted, 0)

        # column_rle: for VLCS only.
        if self.key_format == 'r' and self.value_format != '8t':
            # We deleted a key, so two RLE cells adding to nrows - 1 total.
            self.assertEqual(column_rle, nrows - 3)
        else:
            self.assertEqual(column_rle, 0)

        # column_tws: for FLCS only.
        if self.key_format == 'r' and self.value_format == '8t':
            if self.oldest > 20:
                # Only the deletions show.
                self.assertEqual(column_tws, 2)
            else:
                self.assertEqual(column_tws, nrows * 2)
        else:
            self.assertEqual(column_tws, 0)

        # overflow: two keys and one value, so 3 for rows, 1 for VLCS, 0 for FLCS.
        if self.key_format == 'S':
            self.assertEqual(overflow, 3)
        elif self.value_format == '8t':
            self.assertEqual(overflow, 0)
        else:
            self.assertEqual(overflow, 1)

if __name__ == '__main__':
    wttest.run()
