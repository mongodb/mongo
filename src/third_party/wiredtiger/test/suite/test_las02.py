#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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

from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet

def timestamp_str(t):
    return '%x' % t

# test_las02.py
# Test that truncate with lookaside entries and timestamps gives expected results.
class test_las02(wttest.WiredTigerTestCase):
    # Force a small cache.
    def conn_config(self):
        return 'cache_size=50MB,log=(enabled)'

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records, we'll hang if the lookaside table isn't working.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        session.begin_transaction('read_timestamp=' + timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.rollback_transaction()
        self.assertEqual(count, nrows)

    def test_las(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        nrows = 10000

        # Create a table without logging to ensure we get "skew_newest" lookaside eviction behavior.
        uri = "table:las02_main"
        ds = SimpleDataSet(
            self, uri, 0, key_format="S", value_format="S", config='log=(enabled=false)')
        ds.populate()

        uri2 = "table:las02_extra"
        ds2 = SimpleDataSet(self, uri2, 0, key_format="S", value_format="S")
        ds2.populate()

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1) +
            ',stable_timestamp=' + timestamp_str(1))

        bigvalue = "aaaaa" * 100
        self.large_updates(uri, bigvalue, ds, nrows / 3, 1)

        # Check that all updates are seen
        self.check(bigvalue, uri, nrows / 3, 1)

        # Check to see lookaside working with old timestamp
        bigvalue2 = "ddddd" * 100
        self.large_updates(uri, bigvalue2, ds, nrows, 100)

        # Check that the new updates are only seen after the update timestamp
        self.check(bigvalue, uri, nrows / 3, 1)
        self.check(bigvalue2, uri, nrows, 100)

        # Force out most of the pages by updating a different tree
        self.large_updates(uri2, bigvalue, ds2, nrows, 100)

        # Now truncate half of the records
        self.session.begin_transaction()
        end = self.session.open_cursor(uri)
        end.set_key(ds.key(nrows / 2))
        self.session.truncate(None, None, end)
        end.close()
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(200))

        # Check that the truncate is visible after commit
        self.check(bigvalue2, uri, nrows / 2, 200)

        # Repeat earlier checks
        self.check(bigvalue, uri, nrows / 3, 1)
        self.check(bigvalue2, uri, nrows, 100)

if __name__ == '__main__':
    wttest.run()
