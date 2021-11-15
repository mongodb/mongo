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
#
# test_prepare04.py
#   Prepare: Update and read operations generate prepared conflict error.
#

import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

class test_prepare04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare_cursor'
    uri = 'table:' + tablename
    session_config = 'isolation=snapshot'

    before_ts = timestamp_str(150)
    prepare_ts = timestamp_str(200)
    after_ts = timestamp_str(250)

    types = [
        ('col', dict(extra_config=',log=(enabled=false),key_format=r')),
        ('col-fix', dict(extra_config=',log=(enabled=false),key_format=r,value_format=8t')),
        ('lsm', dict(extra_config=',log=(enabled=false),type=lsm')),
        ('row', dict(extra_config=',log=(enabled=false)')),
    ]

    # Various begin_transaction config
    txncfg = [
        ('before_ts', dict(txn_config='isolation=snapshot,read_timestamp=' + before_ts, after_ts=False)),
        ('after_ts', dict(txn_config='isolation=snapshot,read_timestamp=' + after_ts, after_ts=True)),
        ('no_ts', dict(txn_config='isolation=snapshot', after_ts=True)),
    ]

    preparecfg = [
        ('ignore_false', dict(ignore_config=',ignore_prepare=false', ignore=False)),
        ('ignore_true', dict(ignore_config=',ignore_prepare=true', ignore=True)),
    ]
    conn_config = 'log=(enabled)'

    scenarios = make_scenarios(types, txncfg, preparecfg)

    def test_prepare_conflict(self):
        self.session.create(self.uri,
            'key_format=i,value_format=i' + self.extra_config)
        c = self.session.open_cursor(self.uri)

        # Insert keys 1..100 each with timestamp=key, in some order
        orig_keys = list(range(1, 101))
        keys = orig_keys[:]
        random.shuffle(keys)

        k = 1
        self.session.begin_transaction()
        c[k] = 1
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(100))

        # Everything up to and including timestamp 100 has been committed.
        self.assertTimestampsEqual(self.conn.query_timestamp(), timestamp_str(100))

        # Bump the oldest timestamp, we're not going back...
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(100))

        # make prepared updates.
        k = 1
        self.session.begin_transaction('isolation=snapshot')
        c.set_key(1)
        c.set_value(2)
        c.update()
        self.session.prepare_transaction('prepare_timestamp=' + self.prepare_ts)
        conflictmsg = '/conflict between concurrent operations/'
        preparemsg = '/conflict with a prepared update/'

        #'''
        # Verify data visibility from a different session/transaction.
        s_other = self.conn.open_session()
        c_other = s_other.open_cursor(self.uri, None)
        s_other.begin_transaction(self.txn_config + self.ignore_config)
        c_other.set_key(1)
        if self.ignore == False and self.after_ts == True:
            # Make sure we get the expected prepare conflict message.
            self.assertRaisesException(wiredtiger.WiredTigerError, lambda:c_other.search(), preparemsg)
        else:
            c_other.search()
            self.assertTrue(c_other.get_value() == 1)

        c_other.set_value(3)

        # Make sure we detect the conflict between operations.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda:c_other.update(), conflictmsg)
        s_other.rollback_transaction()

        self.session.timestamp_transaction('commit_timestamp=' + timestamp_str(300))
        self.session.timestamp_transaction('durable_timestamp=' + timestamp_str(300))
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
