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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs26.py
# Test that changes overlapping variable-length column store RLE groups don't lose or corrupt data.
# (The concern doesn't exist for row stores, so while this test could be run for row stores there's
# little benefit to doing so; thus, no row-store scenarios are generated.)
#
# This works by writing batches of duplicate values, then overwriting them with new batches,
# all with relatively prime counts so the ranges overlap. It also tests the cases where not
# all the old or new value batches exist.
#
# It checks that the immediately written values can be read back, but the interesting part is
# whether they read back correctly after forcing eviction, which will RLE-encode the duplicates
# and read them back. There are many opportunities for the interaction betwee RLE groups and
# history store accesses to go off the rails.
class test_hs26(wttest.WiredTigerTestCase):
    conn_config = ''

    # We control the duplication of values by appending a number computed from the key.
    # Because the keys are 1..N (not 0..N-1), to get aligned RLE groups the suffix is
    # computed as (key - 1) // value_modulus. This way, if value_modulus is e.g. 7, we
    # get the first 7 keys (1-7) using the same suffix.
    #
    # It would not be _wrong_ to use key // value modulus, but it makes it much more
    # difficult to reason about which RLE groups are overlapping where if the first RLE
    # group is shorter than the others by 1. So we don't do that.
    #
    # The cases where the RLE group is mismatched by exactly 1 are particularly likely to
    # have issues, so we want to be sure to exercise those cases. Any pair of relatively
    # prime integers will generate these cases as long as there are enough keys (e.g. 7
    # and 13 generate one at 14 and the other at 78) but we also want to encounter these
    # cases relative to the number of keys written, both the shorter and longer numbers.
    #
    # Consequently I've picked 103 for one key count (between multiples of 17 and 13) and
    # 209 for the other (between multiples of 13 and 7). The ratio of the two key counts
    # doesn't signify much but it's close to 2x on general principles.
    #
    # Other cases of overlapping the key count are still interesting so we still generate
    # the full product of the scenarios.

    visibility_values = [
        ('not_globally_visible', dict(ts1_globally_visible=False)),
        ('globally_visible', dict(ts1_globally_visible=True)),
    ]
    nrows_values = [
        ('more', dict(nrows_1=103, nrows_2=211)),
        ('same', dict(nrows_1=211, nrows_2=211)),
        ('less', dict(nrows_1=211, nrows_2=103)),
    ]
    value_modulus_1_values = [
        ('7', dict(value_modulus_1=7)),
        ('13', dict(value_modulus_1=13)),
        ('17', dict(value_modulus_1=17)),
    ]
    value_modulus_2_values = [
        ('7', dict(value_modulus_2=7)),
        ('13', dict(value_modulus_2=13)),
        ('17', dict(value_modulus_2=17)),
    ]

    scenarios = make_scenarios(visibility_values,
        nrows_values, value_modulus_1_values, value_modulus_2_values)

    value_1 = 'a' * 500
    value_2 = 'd' * 500

    timestamp_1 = 2
    timestamp_2 = 100

    # Generate the value for a key.
    def make_value(self, key, base_value, value_modulus):
        return base_value + str((key - 1) // value_modulus)

    # Write nrows records, using value as the base value string.
    def make_updates(self, uri, ds, value, value_modulus, nrows, commit_ts):
        session = self.session
        cursor = session.open_cursor(uri)
        session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = self.make_value(i, value, value_modulus)
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    # Figure the expected value for a key, based on the read time.
    # - If the read time is timestamp_1, we should see value_1.
    # - If the read time is timestamp_2, we should see value_2, except if nrows_2 < nrows_1,
    # in which case we should see value_1 for keys past nrows_1.
    def expected_value(self, key, readtime):
        if readtime == self.timestamp_1:
            return self.make_value(key, self.value_1, self.value_modulus_1)
        elif readtime == self.timestamp_2:
            if self.nrows_2 < self.nrows_1 and key > self.nrows_2:
                return self.make_value(key, self.value_1, self.value_modulus_1)
            else:
                return self.make_value(key, self.value_2, self.value_modulus_2)
        else:
            self.prout("expected_value: Unexpected readtime {}".format(readtime))
            self.assertTrue(False)
            return None

    # Return the number of keys we expect, based on the read time.
    # - If the read time is timestamp_1, we should see nrows_1.
    # - If the read time is timestamp_2, we should see max(nrows_1, nrows_2).
    def expected_numvalues(self, readtime):
        if readtime == self.timestamp_1:
            return self.nrows_1
        elif readtime == self.timestamp_2:
            return max(self.nrows_1, self.nrows_2)
        else:
            self.prout("expected_numvalues: Unexpected readtime {}".format(readtime))
            self.assertTrue(False)
            return None

    # Check that we got the values we expected. In particular, also make sure that
    # we get the expected number of values back. Expect the values that should be
    # there at readtime; if explicit_read_ts is set, open a transaction at that
    # timestamp.
    def check(self, session, uri, readtime, explicit_read_ts=-1):
        if explicit_read_ts != -1:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(explicit_read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            # Count is key - 1, so pass count + 1 as key.
            self.assertEqual(v, self.expected_value(count + 1, readtime))
            count += 1
        if explicit_read_ts != -1:
            session.rollback_transaction()
        self.assertEqual(count, self.expected_numvalues(readtime))
        cursor.close()

    def test_hs(self):

        # Create a file that contains active history (content newer than the oldest timestamp).
        table_uri = 'table:hs26'
        ds = SimpleDataSet(
            self, table_uri, 0, key_format='r', value_format='S', config='log=(enabled=false)')
        ds.populate()
        self.session.checkpoint()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write the first set of values at timestamp_1.
        self.make_updates(ds.uri, ds, self.value_1, self.value_modulus_1, self.nrows_1, self.timestamp_1)

        # Optionally make the first set of values globally visible (and stable).
        if self.ts1_globally_visible:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.timestamp_1) +
                ',stable_timestamp=' + self.timestamp_str(self.timestamp_1))

        # Create a long running read transaction in a separate session.
        session_read = self.conn.open_session()
        session_read.begin_transaction('read_timestamp=' + self.timestamp_str(self.timestamp_1))

        # Check that the initial writes (at timestamp_1) are seen.
        self.check(session_read, ds.uri, self.timestamp_1)

        # Write different values at a later timestamp.
        self.make_updates(ds.uri, ds, self.value_2, self.value_modulus_2, self.nrows_2, self.timestamp_2)

        # Check that the new updates are only seen after the update timestamp.
        self.check(self.session, ds.uri, self.timestamp_1, self.timestamp_1)
        self.check(self.session, ds.uri, self.timestamp_2, self.timestamp_2)

        # Now forcibly evict, so that all the pages are RLE-encoded and then read back in.
        # There doesn't seem to be any way to just forcibly evict an entire table, so what
        # I'm going to do is assume that each page can hold at least 41 values, and evict
        # every 41st key. If this evicts pages repeatedly it won't really hurt anything,
        # just waste some time.

        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        for k in range(1, max(self.nrows_1, self.nrows_2) + 1, 41):
            # Search the key to evict it.
            v = evict_cursor[ds.key(k)]
            xv = self.expected_value(k, self.timestamp_2)
            self.assertEqual(v, xv)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Using the long running read transaction, check that the correct data can still be read.
        # It should see all the updates at timestamp_1.
        self.check(session_read, ds.uri, self.timestamp_1)
        session_read.rollback_transaction()

        # Also check that the most recent transaction has the later data.
        self.check(self.session, ds.uri, self.timestamp_2, self.timestamp_2)

if __name__ == '__main__':
    wttest.run()
