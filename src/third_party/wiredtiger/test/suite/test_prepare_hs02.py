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
# test_prepare_hs02.py
#   Prepare updates can be resolved for both commit // rollback operations.
#

from helper import copy_wiredtiger_home
import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_prepare_hs02(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare_cursor'
    uri = 'table:' + tablename

    types = [
        ('col', dict(s_config='key_format=r,value_format=i,log=(enabled=false)')),
        ('col-fix', dict(s_config='key_format=r,value_format=8t,log=(enabled=false)')),
        ('row', dict(s_config='key_format=i,value_format=i,log=(enabled=false)')),
        ('lsm', dict(s_config='key_format=i,value_format=i,log=(enabled=false),type=lsm')),
    ]

    # Transaction end types
    txn_end = [
        ('txn_commit', dict(txn_commit=True)),
        ('txn_rollback', dict(txn_commit=False)),
    ]

    scenarios = make_scenarios(types, txn_end)

    def test_prepare_conflict(self):
        self.session.create(self.uri, self.s_config)
        c = self.session.open_cursor(self.uri)

        # Insert keys 1..100 each with timestamp=key, in some order
        orig_keys = list(range(1, 101))
        keys = orig_keys[:]
        random.shuffle(keys)

        # Scenario: 1
        # Check insert operation
        self.session.begin_transaction()
        c[1] = 1
        # update the value with in this transaction
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100))
        if self.txn_commit == True:
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(101) + ',durable_timestamp=' + self.timestamp_str(101))
        else:
            self.session.rollback_transaction()

        # Trigger a checkpoint, which could trigger reconciliation
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(150))
        self.session.checkpoint()

        # Scenario: 2
        # Check update operation
        #   update a existing key.
        #   update a newly inserted key with in this transaction
        self.session.begin_transaction()
        # update a committed value, key 1 is inserted above.
        c[1] = 2
        # update a uncommitted value, insert and update a key.
        c[2] = 1
        c[2] = 2
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(200))
        if self.txn_commit == True:
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(201) + ',durable_timestamp=' + self.timestamp_str(201))
        else:
            self.session.rollback_transaction()

        # Trigger a checkpoint, which could trigger reconciliation
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(250))
        self.session.checkpoint()

        # Scenario: 3
        # Check remove operation
        #   remove an existing key.
        #   remove a previously updated key.
        #   remove a newly inserted and updated key.
        self.session.begin_transaction()
        # update a committed value, key 1 is inserted above.
        c.set_key(1)
        c.remove()
        c.set_key(2)
        c.remove()
        c[3] = 1
        c[3] = 2
        c.set_key(3)
        c.remove()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(300))
        if self.txn_commit == True:
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(301) + ',durable_timestamp=' + self.timestamp_str(301))
        else:
            self.session.rollback_transaction()

        # commit some keys, to generate the update chain subsequently.
        self.session.begin_transaction()
        c[1] = 1
        c[2] = 1
        c[3] = 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(302))

        # Trigger a checkpoint, which could trigger reconciliation
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(350))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(350))
        self.session.checkpoint()

        #Scenario: 4
        # Check update operation on a checkpointed key. Re-open is to facilitate
        # creating the modify update_chain for key instead of insert update
        # chain.
        self.reopen_conn()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(350))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(350))

        self.session.create(self.uri, self.s_config)
        cur = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cur[1] = 2
        cur[2] = 2
        cur[3] = 2
        # Update a key twice
        cur[2] = 3
        # Remove a updated key
        cur.set_key(3)
        cur.remove()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(400))
        if self.txn_commit == True:
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(401) + ',durable_timestamp=' + self.timestamp_str(401))
        else:
            self.session.rollback_transaction()

        # Trigger a checkpoint, which could trigger reconciliation
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(450))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(450))
        self.session.checkpoint()

        cur.close()
        self.session.close()

if __name__ == '__main__':
    wttest.run()
