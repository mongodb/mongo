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

from helper import copy_wiredtiger_home
import os
import wiredtiger, wttest

# test_bug018.py
#   JIRA WT-3590: if writing table data fails during close then tables
# that were updated within the same transaction could get out of sync with
# each other.
class test_bug018(wttest.WiredTigerTestCase):
    '''Test closing/reopening/recovering tables when writes fail'''

    conn_config = 'log=(enabled)'

    def setUp(self):
        # This test uses Linux-specific code so skip on any other system.
        if os.name != 'posix' or os.uname()[0] != 'Linux':
            self.skipTest('Linux-specific test skipped on ' + os.name)
        super(test_bug018, self).setUp()

    def create_table(self, uri):
        self.session.create(uri, 'key_format=S,value_format=S')
        return self.session.open_cursor(uri)

    def test_bug018(self):
        '''Test closing multiple tables'''
        basename = 'bug018.'
        baseuri = 'file:' + basename
        c1 = self.create_table(baseuri + '01.wt')
        c2 = self.create_table(baseuri + '02.wt')

        self.session.begin_transaction()
        c1['key'] = 'value'
        c2['key'] = 'value'
        self.session.commit_transaction()

        # Simulate a write failure by closing the file descriptor for the second
        # table out from underneath WiredTiger.  We do this right before
        # closing the connection so that the write error happens during close
        # when writing out the final data.  Allow table 1 to succeed and force
        # an error writing out table 2.
        #
        # This is Linux-specific code to figure out the file descriptor.
        for f in os.listdir('/proc/self/fd'):
            try:
                if os.readlink('/proc/self/fd/' + f).endswith(basename + '02.wt'):
                    os.close(int(f))
            except OSError:
                pass

        # Expect an error and messages, so turn off stderr checking.
        with self.expectedStderrPattern(''):
            try:
                self.close_conn()
            except wiredtiger.WiredTigerError:
                self.conn = None

        # Make a backup for forensics in case something goes wrong.
        backup_dir = 'BACKUP'
        copy_wiredtiger_home('.', backup_dir, True)

        # After reopening and running recovery both tables should be in
        # sync even though table 1 was successfully written and table 2
        # had an error on close.
        self.open_conn()
        c1 = self.session.open_cursor(baseuri + '01.wt')
        c2 = self.session.open_cursor(baseuri + '02.wt')
        self.assertEqual(list(c1), list(c2))

if __name__ == '__main__':
    wttest.run()
