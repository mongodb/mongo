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
# test_util12.py
#	Utilities: wt write
#

import os, struct
import wiredtiger, wttest

class test_txn01(wttest.WiredTigerTestCase):
    tablename = 'test_txn01'
    nentries = 10000
    session_params = 'key_format=r,value_format=S'

    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create,' +
                ('error_prefix="%s: ",' % self.shortid()) +
                'transactional,')
        self.pr(`conn`)
        return conn

    def check_count(self, expected):
        s = self.conn.open_session()
        s.checkpoint("snapshot=test")
        try:
            cursor = s.open_cursor('table:' + self.tablename, None, "snapshot=test")
            count = 0
            for r in cursor:
                count += 1
            cursor.close()
        finally:
            s.close()
        self.assertEqual(count, expected)

    def test_write(self):
        self.session.create('table:' + self.tablename, self.session_params)
        self.check_count(0)
        self.session.begin_transaction()
        cursor = self.session.open_cursor('table:' + self.tablename, None, "append")
        for i in xrange(self.nentries):
            if i > 0 and (i * 10) % self.nentries == 0:
                self.check_count(i - self.nentries / 10)
                self.session.commit_transaction()
                self.session.begin_transaction()
                cursor = self.session.open_cursor('table:' + self.tablename, None, "append")
            cursor.set_value(("value" + str(i)) * 100)
            cursor.insert()
        cursor.close()
        self.check_count(self.nentries - self.nentries / 10)
        self.session.commit_transaction()
        self.check_count(self.nentries)

if __name__ == '__main__':
    wttest.run()
