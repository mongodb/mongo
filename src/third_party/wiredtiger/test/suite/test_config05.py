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

import wiredtiger, wttest

# test_config05.py
#    Test multiple connection opens
class test_config05(wttest.WiredTigerTestCase):
    table_name1 = 'test_config05'
    nentries = 100

    # Each test needs to set up its connection in its own way,
    # so override these methods to do nothing
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def close_conn(self):
        if self.conn != None:
            self.conn.close()
            self.conn = None
        if hasattr(self, 'conn2') and self.conn2 != None:
            self.conn2.close()
            self.conn2 = None

    def populate(self, session):
        """
        Create entries using key=string, value=string
        """
        create_args = 'key_format=S,value_format=S'
        session.create("table:" + self.table_name1, create_args)
        cursor = session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor[str(1000000 + i)] = 'value' + str(i)
        cursor.close()

    def verify_entries(self, session):
        """
        Verify all entries created in populate()
        """
        cursor = session.open_cursor('table:' + self.table_name1, None, None)
        i = 0
        for key, value in cursor:
            self.assertEqual(key, str(1000000 + i))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def test_one(self):
        self.conn = self.wiredtiger_open('.', 'create')
        self.session = self.conn.open_session(None)
        self.populate(self.session)
        self.verify_entries(self.session)

    def test_one_session(self):
        self.conn = self.wiredtiger_open('.', 'create,session_max=1')
        self.session = self.conn.open_session(None)
        self.populate(self.session)
        self.verify_entries(self.session)

    def test_too_many_sessions(self):
        self.conn = self.wiredtiger_open('.', 'create,session_max=1')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: [self.conn.open_session(None) for i in range(100)],
            '/out of sessions/')

    def test_exclusive_create(self):
        self.conn = self.wiredtiger_open('.', 'create,exclusive')
        self.conn.close()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', 'exclusive'),
            '/WiredTiger database already exists/')

    def test_multi_create(self):
        self.conn = self.wiredtiger_open('.', 'create')
        self.session = self.conn.open_session(None)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', 'create'),
            '/WiredTiger database is already being managed/')

if __name__ == '__main__':
    wttest.run()
