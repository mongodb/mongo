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
#
# test_timestamp02.py
#   Timestamps: basic semantics
#

import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

class test_timestamp02(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp02'
    uri = 'table:' + tablename

    scenarios = make_scenarios([
        ('col', dict(extra_config=',key_format=r')),
        ('lsm', dict(extra_config=',type=lsm')),
        ('row', dict(extra_config='')),
    ])

    conn_config = 'log=(enabled)'

    # Check that a cursor (optionally started in a new transaction), sees the
    # expected values.
    def check(self, session, txn_config, expected):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri, None)
        actual = dict((k, v) for k, v in c if v != 0)
        self.assertTrue(actual == expected)
        # Search for the expected items as well as iterating
        for k, v in expected.iteritems():
            self.assertEqual(c[k], v, "for key " + str(k))
        c.close()
        if txn_config:
            session.commit_transaction()

    def test_basic(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        self.session.create(self.uri,
            'key_format=i,value_format=i' + self.extra_config)
        c = self.session.open_cursor(self.uri)

        # Insert keys 1..100 each with timestamp=key, in some order
        orig_keys = range(1, 101)
        keys = orig_keys[:]
        random.shuffle(keys)

        for k in keys:
            self.session.begin_transaction()
            c[k] = 1
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(k))

        # Don't set a stable timestamp yet.  Make sure we can read with
        # a timestamp before the stable timestamp has been set.
        # Now check that we see the expected state when reading at each
        # timestamp
        for i, t in enumerate(orig_keys):
            self.check(self.session, 'read_timestamp=' + timestamp_str(t),
                dict((k, 1) for k in orig_keys[:i+1]))

        # Everything up to and including timestamp 100 has been committed.
        self.assertTimestampsEqual(self.conn.query_timestamp(), timestamp_str(100))

        # Bump the oldest timestamp, we're not going back...
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(100))

        # Update them and retry.
        random.shuffle(keys)
        for k in keys:
            self.session.begin_transaction()
            c[k] = 2
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(k + 100))

        # Everything up to and including timestamp 200 has been committed.
        self.assertTimestampsEqual(self.conn.query_timestamp(), timestamp_str(200))

        # Test that we can manually move the commit timestamp back
        self.conn.set_timestamp('commit_timestamp=' + timestamp_str(150))
        self.assertTimestampsEqual(self.conn.query_timestamp(), timestamp_str(150))
        self.conn.set_timestamp('commit_timestamp=' + timestamp_str(200))

        # Now the stable timestamp before we read.
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(200))

        for i, t in enumerate(orig_keys):
            self.check(self.session, 'read_timestamp=' + timestamp_str(t + 100),
                dict((k, (2 if j <= i else 1)) for j, k in enumerate(orig_keys)))

        # Bump the oldest timestamp, we're not going back...
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(200))

        # Remove them and retry
        random.shuffle(keys)
        for k in keys:
            self.session.begin_transaction()
            del c[k]
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(k + 200))

        # We have to continue to advance the stable timestamp before reading.
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(300))
        for i, t in enumerate(orig_keys):
            self.check(self.session, 'read_timestamp=' + timestamp_str(t + 200),
                dict((k, 2) for k in orig_keys[i+1:]))

    def test_read_your_writes(self):
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        self.session.create(self.uri,
            'key_format=i,value_format=i' + self.extra_config)
        c = self.session.open_cursor(self.uri)

        k = 10
        c[k] = 0

        self.session.begin_transaction('read_timestamp=10')
        self.session.timestamp_transaction('commit_timestamp=20')
        c[k] = 1
        # We should see the value we just inserted
        self.assertEqual(c[k], 1)
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
