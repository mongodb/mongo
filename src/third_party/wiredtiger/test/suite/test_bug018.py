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
from suite_subprocess import suite_subprocess
import os
import wiredtiger, wttest

# test_bug018.py
#   JIRA WT-3590: if writing table data fails during close then tables
# that were updated within the same transaction could get out of sync with
# each other.
class test_bug018(wttest.WiredTigerTestCase, suite_subprocess):
    '''Test closing/reopening/recovering tables when writes fail'''

    conn_config = 'log=(enabled)'
    basename = 'bug018.'
    baseuri = 'file:' + basename
    flist = []
    uri1 = baseuri + '01.wt'
    uri2 = baseuri + '02.wt'

    def setUp(self):
        # This test uses Linux-specific code so skip on any other system.
        if os.name != 'posix' or os.uname()[0] != 'Linux':
            self.skipTest('Linux-specific test skipped on ' + os.name)
        super(test_bug018, self).setUp()

    def close_files(self):
        for f in self.flist:
            f.close()

    def open_files(self):
        numfiles = 6
        dir = self.conn.get_home()
        for i in range(1, numfiles):
            fname = dir + '/file.' + str(i)
            self.flist.append(open(fname, 'w'))

    def create_table(self, uri):
        self.session.create(uri, 'key_format=S,value_format=S')
        return self.session.open_cursor(uri)

    def subprocess_bug018(self):
        '''Test closing multiple tables'''
        # The first thing we do is open several files. We will close them later. The reason is
        # that sometimes, without that, this test would fail to report an error as expected. We
        # hypothesize, but could not prove (nor reproduce under strace), that after closing the
        # file descriptor that an internal thread would open a file, perhaps a pre-allocated log
        # file, and then would open the file descriptor we just closed. So on close, instead of
        # getting an error, we would actually write to the wrong file.
        #
        # So we'll open some files now, and then close them before closing the one of interest to
        # the test so that any stray internal file opens will use the file descriptor of one of
        # the earlier files we just closed.
        self.open_files()
        c1 = self.create_table(self.uri1)
        c2 = self.create_table(self.uri2)

        self.session.begin_transaction()
        c1['key'] = 'value'
        c2['key'] = 'value'
        self.session.commit_transaction()

        self.close_files()
        # Simulate a write failure by closing the file descriptor for the second
        # table out from underneath WiredTiger.  We do this right before
        # closing the connection so that the write error happens during close
        # when writing out the final data.  Allow table 1 to succeed and force
        # an error writing out table 2.
        #
        # This is Linux-specific code to figure out the file descriptor.
        for f in os.listdir('/proc/self/fd'):
            try:
                if os.readlink('/proc/self/fd/' + f).endswith(self.basename + '02.wt'):
                    os.close(int(f))
            except OSError:
                pass

        # Expect an error and messages, so turn off stderr checking.
        with self.expectedStderrPattern(''):
            try:
                self.close_conn()
            except wiredtiger.WiredTigerError:
                self.conn = None

    def test_bug018(self):
        '''Test closing multiple tables'''

        self.close_conn()
        subdir = 'SUBPROCESS'
        [ignore_result, new_home_dir] = self.run_subprocess_function(subdir,
            'test_bug018.test_bug018.subprocess_bug018')

        # Make a backup for forensics in case something goes wrong.
        backup_dir = 'BACKUP'
        copy_wiredtiger_home(self, new_home_dir, backup_dir, True)

        # After reopening and running recovery both tables should be in
        # sync even though table 1 was successfully written and table 2
        # had an error on close.
        self.open_conn(new_home_dir)

        results1 = list(self.session.open_cursor(self.uri1))

        # It's possible the second table can't even be opened.
        # That can happen only if the root page was not pushed out.
        # We can't depend on the text of a particular error message to be
        # emitted, so we'll just ignore the error.
        self.captureerr.check(self)     # check there is no error output so far
        try:
            results2 = list(self.session.open_cursor(self.uri2))
        except:
            # Make sure there's some error, but we don't care what.
            self.captureerr.checkAdditionalPattern(self, '.')
            results2 = []
        self.assertEqual(results1, results2)

if __name__ == '__main__':
    wttest.run()
