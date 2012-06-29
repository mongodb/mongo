#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
# test_txn02.py
#   Transactions: commits and rollbacks
#

import os, struct
import wiredtiger, wttest
from wtscenario import multiply_scenarios, number_scenarios

class test_txn02(wttest.WiredTigerTestCase):
    tablename = 'test_txn02'
    uri = 'table:' + tablename

    types = [
        ('row', dict(tabletype='row',
                    create_params = 'key_format=i,value_format=i')),
        ('var', dict(tabletype='var',
                    create_params = 'key_format=r,value_format=i')),
        ('fix', dict(tabletype='fix',
                    create_params = 'key_format=r,value_format=8t')),
    ]
    op1s = [
        ('i4', dict(op1=('insert', 4))),
        ('r1', dict(op1=('remove', 1))),
        ('u10', dict(op1=('update', 10))),
    ]
    op2s = [
        ('i6', dict(op2=('insert', 6))),
        ('r4', dict(op2=('remove', 4))),
        ('u4', dict(op2=('update', 4))),
    ]
    op3s = [
        ('i12', dict(op3=('insert', 12))),
        ('r4', dict(op3=('remove', 4))),
        ('u4', dict(op3=('update', 4))),
    ]
    op4s = [
        ('i14', dict(op4=('insert', 14))),
        ('r12', dict(op4=('remove', 12))),
        ('u12', dict(op4=('update', 12))),
    ]
    txn1s = [('t1c', dict(txn1='commit')), ('t1r', dict(txn1='rollback'))]
    txn2s = [('t2c', dict(txn2='commit')), ('t2r', dict(txn2='rollback'))]
    txn3s = [('t3c', dict(txn3='commit')), ('t3r', dict(txn3='rollback'))]
    txn4s = [('t4c', dict(txn4='commit')), ('t4r', dict(txn4='rollback'))]

    scenarios = number_scenarios(multiply_scenarios('.', types,
        op1s, txn1s, op2s, txn2s, op3s, txn3s, op4s, txn4s)) # [:1]

    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create,' +
                ('error_prefix="%s: ",' % self.shortid()) +
                'transactional,')
        self.pr(`conn`)
        self.session2 = conn.open_session()
        return conn

    def check(self, session, txn_config, expected):
        if txn_config:
            session.begin_transaction(txn_config)
        c = session.open_cursor(self.uri, None)
        actual = dict((k, v) for k, v in c if v != 0)
        # Search for the expected items as well as iterating
        for k, v in expected.iteritems():
            c.set_key(k)
            c.search()
            self.assertEqual(c.get_value(), v)
        c.close()
        if txn_config:
            session.commit_transaction()
        self.assertEqual(actual, expected)

    def test_ops(self):
        self.session.create(self.uri, self.create_params)
        # Set up the table with entries for 1 and 10
        c = self.session.open_cursor(self.uri, None)
        c.set_value(1)
        c.set_key(1)
        c.insert()
        c.set_key(10)
        c.insert()
        c.close()
        expected = {1:1, 10:1}

        ops = (self.op1, self.op2, self.op3, self.op4)
        txns = (self.txn1, self.txn2, self.txn3, self.txn4)
        # print ', '.join('%s(%d)[%s]' % (ok[0], ok[1], txn)
        #                 for ok, txn in zip(ops, txns))
        for i, ot in enumerate(zip(ops, txns)):
            self.session.begin_transaction()
            c = self.session.open_cursor(self.uri, None, 'overwrite')
            ok, txn = ot
            op, k = ok
            # print '%s(%d)[%s]' % (ok[0], ok[1], txn)
            # We use the overwrite config so insert can update as needed.
            if op == 'insert' or op == 'update':
                c.set_key(k)
                c.set_value(i + 2)
                c.insert()
                # A snapshot transaction should not see the changes
                self.check(self.session2, "isolation=snapshot", expected)
                if txn == 'commit':
                    expected[k] = i + 2
            elif op == 'remove':
                c.set_key(k)
                c.remove()
                # A snapshot transaction should not see the changes
                self.check(self.session2, "isolation=snapshot", expected)
                if txn == 'commit' and k in expected:
                    del expected[k]
            else:
                print "UNKNOWN op", op
            if txn == 'commit':
                # The transaction should see its own changes
                self.check(self.session, None, expected)
                # A read-uncommitted transaction should see the changes already
                self.check(self.session2,
                    "isolation=read-uncommitted", expected)
                self.session.commit_transaction()
            elif txn == 'rollback':
                self.session.rollback_transaction()
            else:
                print "UNKNOWN op", op

            # The change should be (in)visible in the same session
            self.check(self.session, None, expected)
            # The change should be (in)visible to snapshot transactions
            self.check(self.session2, "isolation=snapshot", expected)
        self.session.drop(self.uri)

if __name__ == '__main__':
    wttest.run()
