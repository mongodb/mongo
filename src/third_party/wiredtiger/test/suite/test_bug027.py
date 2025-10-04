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
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet

# test_bug027.py
# Attempt to create a snapshot with more than 256 transactions in it.
class test_bug(wttest.WiredTigerTestCase):
    conn_config = 'session_max=512'

    key_format='i'
    value_format='S'

    def check(self, ds, nrows, value, lastvalue):
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction()
        for k, v in cursor:
            if k == ds.key(nrows):
                self.assertEqual(v, lastvalue)
            else:
                self.assertEqual(v, value)
        self.session.rollback_transaction()
        cursor.close()

    def test_bug(self):
        uri = "table:bug026"
        nrows = 1000
        ntxns = 500

        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        value_c = "ccccc" * 100

        # Write some data.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value_a
        self.session.commit_transaction()
        self.session.checkpoint()

        # Create a bunch of transactions and leave all but one hanging.
        sessions = {}
        cursors = {}
        for i in range(1, ntxns + 1):
            sessions[i] = self.conn.open_session()
            cursors[i] = sessions[i].open_cursor(uri)
            sessions[i].begin_transaction()
            cursors[i][ds.key(i)] = value_b
        self.session.begin_transaction()
        cursor[ds.key(nrows)] = value_c
        self.session.commit_transaction()
        self.session.checkpoint()

        # Should not see value_b.
        self.check(ds, nrows, value_a, value_c)

        # Now crash.
        simulate_crash_restart(self, ".", "RESTART")

        # Should still not see value_b.
        self.check(ds, nrows, value_a, value_c)

