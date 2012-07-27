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

import wiredtiger, wttest
from datetime import datetime
from helper import simple_populate, simple_populate_check, simple_populate_check

# test_cursor_random.py
#    Cursor next_random operations
class Test_Cursor_Random(wttest.WiredTigerTestCase):
    nentries = 10

    scenarios = [
        ('row', dict(tablekind='row', uri='table', tablename='one')),
        ('col', dict(tablekind='col', uri='file', tablename='two')),
        ('fix', dict(tablekind='fix', uri='table', tablename='three'))
        ]

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return ""

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        self.session.create(name, args)

    def session_close(self):
        self.session.close()

    def tearDown(self):
        self.session.close()
        super(Test_Cursor_Random, self).tearDown()

    def create_session_and_cursor(self, use_next_random=False, use_random_table_name=False):
        if use_random_table_name:
            rightnow = datetime.now()
            self.tablename = self.tablename + rightnow.strftime("%Y%m%d%H%M%S%f")
        args = self.uri + ":" + self.tablename
        #tablearg = "table:" + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat + self.config_string()
        #print 'Args= %s' % create_args
        self.pr('creating session: ' + create_args)
        self.session_create(args, create_args)
        self.pr('creating cursor')
        config = None
        if use_next_random:
            config = "next_random=true"
        return self.session.open_cursor(args, None, config)

    def verify_populate(self, uri):
        config = "next_random=true"
        cursor = self.session.open_cursor(uri, None, config)
        values_range = [str(x) + ": abcdefghijklmnopqrstuvwxyz" for x in range(0, self.nentries)]
        for i in range(0, self.nentries):
            self.assertEqual(0, cursor.next())
            value = cursor.get_value()
            self.assertTrue(str(value) in values_range)
        cursor.close()

    def test_cursor_random(self):
        """
        We test next_random in this test. We insert values n---m
        in the table. Then using next_random, we retrieve values
        from the database and make sure returned values range within
        n...m
        """
        uri = self.uri + ":" + self.tablename
        config = "next_random=true"
        if self.tablekind == 'row':
            simple_populate(self, uri, "key_format=S", self.nentries)
            self.verify_populate(uri)
        else:#becasue next_random only works in row format, otherwise throws exceptino
            cursor = self.create_session_and_cursor(use_next_random=True)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: cursor.next(),
                    r"")
            cursor.close()
    def test_cursor_random_empty_table(self):
        if self.tablekind == "row":
            cursor = self.create_session_and_cursor(use_next_random=True, \
                    use_random_table_name=True)
            self.assertTrue(cursor.next(), wiredtiger.WT_NOTFOUND)
            cursor.close()

    def test_cursor_random_single_record(self):
        """
        Insert one value and then close the cursor. Then, 
        using next_random we retrieve values and we make
        sure same is returned each time.
        """
        uri = self.uri + ":" + self.tablename
        #cursor = self.create_session_and_cursor()
        #cursor.set_key(self.genkey(1))
        #cursor.set_value(self.genvalue(1))
        #cursor.insert()
        #cursor.close()
        if self.tablekind == 'row':
            simple_populate(self, uri, "key_format=S", self.nentries)
            self.verify_populate(uri)
        else:#becasue next_random only works in row format, otherwise throws exceptino
            cursor = self.create_session_and_cursor(use_next_random=True)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: cursor.next(),
                    r"")
            cursor.close()
    def test_cursor_random_session_recreate(self):
        uri = self.uri + ":" + self.tablename
       #print 'Session closed in test_cursor_random_session_recreate'
        if self.tablekind == 'row':
            simple_populate(self, uri, "key_format=S", self.nentries)
            self.session_close()
            conn = wiredtiger.wiredtiger_open(self.dir)
            session = conn.open_session()
            cursor = session.open_cursor(self.uri + ":" + self.tablename)
            for i in range(1, 5):
                self.assertEqual(cursor.next(), 0)
                self.assertEqual(1, cursor.get_value())
            cursor.close()
