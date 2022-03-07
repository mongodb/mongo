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

import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs02.py
# Test that truncate with history store entries and timestamps gives expected results.
class test_hs02(wttest.WiredTigerTestCase):
    # Force a small cache.
    conn_config = 'cache_size=50MB'

    format_values = [
        ('string-row', dict(key_format='S', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t'))
    ]
    scenarios = make_scenarios(format_values)

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records, we'll hang if the history store table isn't working.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    # expect[] is a list of (value, count) pairs to expect while scanning the table.
    def check(self, uri, expect, read_ts):
        session = self.session
        session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        entry = 0
        (check_value, check_count) = expect[entry]
        count = 0
        for k, v in cursor:
            if count >= check_count:
                self.assertLess(entry, len(expect), "Too many rows returned by cursor")
                entry += 1
                (check_value, check_count) = expect[entry]
                count = 0
            self.assertEqual(v, check_value)
            count += 1
        session.rollback_transaction()
        # If this fails, the cursor didn't return enough rows.
        self.assertEqual(count, check_count)

    def test_hs(self):
        nrows = 10000

        # Create a table.
        uri = "table:hs02_main"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        uri2 = "table:hs02_extra"
        ds2 = SimpleDataSet(
            self, uri2, 0, key_format=self.key_format, value_format=self.value_format)
        ds2.populate()

        if self.value_format == '8t':
            bigvalue = 97
            bigvalue2 = 100
        else:
            bigvalue = "aaaaa" * 100
            bigvalue2 = "ddddd" * 100

        # Commit at timestamp 1.
        self.large_updates(uri, bigvalue, ds, nrows // 3, 1)

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Check that all updates are seen
        self.check(uri, [(bigvalue, nrows // 3)], 1)

        # Check to see the history store working with old timestamp
        self.large_updates(uri, bigvalue2, ds, nrows, 100)

        # Check that the new updates are only seen after the update timestamp
        expect_1 = [(bigvalue, nrows // 3)]
        # In FLCS zeros appear under uncommitted/non-visible updates at the end of the table.
        if self.value_format == '8t':
            expect_1.append((0, nrows - nrows // 3))
        self.check(uri, expect_1, 1)
        self.check(uri, [(bigvalue2, nrows)], 100)

        # Force out most of the pages by updating a different tree
        self.large_updates(uri2, bigvalue, ds2, nrows, 100)

        # Now truncate half of the records
        self.session.begin_transaction()
        end = self.session.open_cursor(uri)
        end.set_key(ds.key(nrows // 2))
        self.session.truncate(None, None, end)
        end.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(200))

        # Check that the truncate is visible after commit
        if self.value_format == '8t':
            expect_200 = [(0, nrows // 2), (bigvalue2, nrows // 2)]
        else:
            expect_200 = [(bigvalue2, nrows // 2)]
        self.check(uri, expect_200, 200)

        # Repeat earlier checks
        self.check(uri, expect_1, 1)
        self.check(uri, [(bigvalue2, nrows)], 100)

if __name__ == '__main__':
    wttest.run()
