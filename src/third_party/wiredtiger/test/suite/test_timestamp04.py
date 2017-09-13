#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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
#
# test_timestamp04.py
#   Timestamps: Test that rollback_to_stable obeys expected visibility rules
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

def timestamp_ret_str(t):
    s = timestamp_str(t)
    if len(s) % 2 == 1:
        s = '0' + s
    return s

class test_timestamp04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp04'
    uri = 'table:' + tablename

    scenarios = make_scenarios([
        ('col_fix', dict(empty=1, extra_config=',key_format=r, value_format=8t')),
        ('col_var', dict(empty=0, extra_config=',key_format=r')),
        #('lsm', dict(empty=0, extra_config=',type=lsm')),
        ('row', dict(empty=0, extra_config='')),
    ])

    # Rollback only works for non-durable tables
    conn_config = 'cache_size=20MB,log=(enabled=false)'

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, expected, missing=False):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri, None)
        if missing == False:
            actual = dict((k, v) for k, v in c if v != 0)
            #print expected
            #print actual
            self.assertEqual(actual, expected)
        # Search for the expected items as well as iterating
        for k, v in expected.iteritems():
            if missing == False:
                self.assertEqual(c[k], v, "for key " + str(k))
            else:
                c.set_key(k)
                if self.empty:
                    # Fixed-length column-store rows always exist.
                    self.assertEqual(c.search(), 0)
                else:
                    self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        c.close()
        if txn_config:
            session.commit_transaction()

    def test_basic(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        # Configure small page sizes to ensure eviction comes through and we have a
        # somewhat complex tree
        self.session.create(self.uri,
            'key_format=i,value_format=i,memory_page_max=32k,leaf_page_max=8k,internal_page_max=8k'
                + self.extra_config)
        c = self.session.open_cursor(self.uri)

        # Insert keys each with timestamp=key, in some order
        key_range = 10000
        keys = range(1, key_range + 1)

        for k in keys:
            self.session.begin_transaction()
            c[k] = 1
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(k))
            # Setup an oldest timestamp to ensure state remains in cache.
            if k == 1:
                self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))

        # Roll back half the timestamps.
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(key_range / 2))
        self.conn.rollback_to_stable()

        # Now check that we see the expected state when reading at each
        # timestamp
        self.check(self.session, 'read_timestamp=' + timestamp_str(key_range / 2),
            dict((k, 1) for k in keys[:(key_range / 2)]))
        self.check(self.session, 'read_timestamp=' + timestamp_str(key_range / 2),
            dict((k, 1) for k in keys[(key_range / 2 + 1):]), missing=True)

        # Bump the oldest timestamp, we're not going back...
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(key_range / 2))

        # Update the values again in preparation for rolling back more
        for k in keys:
            self.session.begin_transaction()
            c[k] = 2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(k + key_range))

        # Now we should have: keys 1-100 with value 2
        self.check(self.session, 'read_timestamp=' + timestamp_str(2 * key_range),
            dict((k, 2) for k in keys[:]))

        # Rollback a quarter of the new commits
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(1 + key_range + key_range / 4))
        self.conn.rollback_to_stable()

        # There should be 50 keys, the first half of which have a value of 2, the
        # second half have a value of 1
        self.check(self.session, 'read_timestamp=' + timestamp_str(2 * key_range),
            dict((k, (2 if j <= (key_range / 4) else 1))
            for j, k in enumerate(keys[:(key_range / 2)])))
        self.check(self.session, 'read_timestamp=' + timestamp_str(key_range / 2),
            dict((k, 1) for k in keys[(1 + key_range / 2):]), missing=True)

if __name__ == '__main__':
    wttest.run()
