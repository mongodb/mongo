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
from wtscenario import multiply_scenarios

class test_txn02(wttest.WiredTigerTestCase):
    tablename = 'test_txn02'
    uri = 'table:' + tablename

    types = [
        ('row', dict(tablekind='row',
                    create_params = 'key_format=i,value_format=i')),
        ('var', dict(tablekind='var',
                    create_params = 'key_format=r,value_format=i')),
        ('fix', dict(tablekind='fix',
                    create_params = 'key_format=r,value_format=1t')),
    ]
    ops = [
        ('op1', dict(ops=[('insert', 1), ('insert', 10)]))
    ]
    scenarios = multiply_scenarios('.', types, ops)

    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create,' +
                ('error_prefix="%s: ",' % self.shortid()) +
                'transactional,')
        self.pr(`conn`)
        return conn

    def test_ops(self):
        self.session.create(self.uri, self.create_params)
        expected = {}
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri, None)
        for op, k in self.ops:
            if op == 'insert':
                c.set_key(k)
                c.set_value(1)
                c.insert()
                expected[k] = 1
        self.session.commit_transaction()
        c = self.session.open_cursor(self.uri, None)
        actual = dict((k, v) for k, v in c if v != 0)
        c.close()
        print "actual", actual
        print "expected", expected
        self.assertEqual(actual, expected)
        self.session.drop(self.uri)

if __name__ == '__main__':
    wttest.run()
