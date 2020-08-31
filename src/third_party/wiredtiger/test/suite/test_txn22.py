#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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
# test_txn22.py
#   Transactions: test salvage with removed

import fnmatch, os, shutil, time
from wtscenario import make_scenarios
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def copy_for_crash_restart(olddir, newdir):
    ''' Simulate a crash from olddir and restart in newdir. '''
    # with the connection still open, copy files to new directory
    shutil.rmtree(newdir, ignore_errors=True)
    os.mkdir(newdir)
    for fname in os.listdir(olddir):
        fullname = os.path.join(olddir, fname)
        # Skip lock file on Windows since it is locked
        if os.path.isfile(fullname) and \
            "WiredTiger.lock" not in fullname and \
            "Tmplog" not in fullname and \
            "Preplog" not in fullname:
            shutil.copy(fullname, newdir)

class test_txn22(wttest.WiredTigerTestCase, suite_subprocess):
    base_config = 'cache_size=1GB'
    conn_config = base_config

    # File to be corrupted
    filename_scenarios = [
        ('WiredTiger', dict(filename='WiredTiger')),
        ('WiredTiger.basecfg', dict(filename='WiredTiger.basecfg')),
        ('WiredTiger.turtle', dict(filename='WiredTiger.turtle')),
        ('WiredTiger.wt', dict(filename='WiredTiger.wt')),
        ('WiredTigerHS.wt', dict(filename='WiredTigerHS.wt')),
        ('test_txn22.wt', dict(filename='test_txn22.wt')),
    ]

    # In many cases, wiredtiger_open without any salvage options will
    # just work.  We list those cases here.
    openable = [
        "removal:WiredTiger.basecfg",
        "removal:WiredTiger.turtle",
    ]

    # The cases for which salvage will not work
    not_salvageable = [
        "removal:WiredTiger.turtle",
        "removal:WiredTiger.wt",
    ]

    scenarios = make_scenarios(filename_scenarios)
    uri = 'table:test_txn22'
    create_params = 'key_format=i,value_format=S'
    nrecords = 1000                                  # records per table.

    def valuegen(self, i):
        return str(i) + 'A' * 1024

    # Insert a list of keys
    def inserts(self, keylist):
        c = self.session.open_cursor(self.uri)
        for i in keylist:
            c[i] = self.valuegen(i)
        c.close()

    def checks(self):
        c = self.session.open_cursor(self.uri)
        gotlist = []
        for key, value in c:
            gotlist.append(key)
            self.assertEqual(self.valuegen(key), value)
        c.close()

    def corrupt_meta(self, homedir):
        # Mark this test has having corrupted files
        self.databaseCorrupted()
        filename = os.path.join(homedir, self.filename)
        os.remove(filename)

    def is_openable(self):
        key = 'removal:' + self.filename
        return key in self.openable

    def is_salvageable(self):
        key = 'removal:' + self.filename
        return key not in self.not_salvageable

    def test_corrupt_meta(self):
        newdir = "RESTART"
        newdir2 = "RESTART2"
        expect = list(range(0, self.nrecords))
        salvage_config = self.base_config + ',salvage=true'

        self.session.create(self.uri, self.create_params)
        self.inserts(expect)

        # Simulate a crash by copying the contents of the directory
        # before closing.  After we corrupt the copy, make another
        # copy of the corrupted directory.
        #
        # The first corrupted copy will be used to run:
        #    wiredtiger_open without salvage flag, followed by:
        #    wiredtiger_open with salvage flag.
        # The second directory will be used to run:
        #    wiredtiger_open with salvage flag first.

        copy_for_crash_restart(self.home, newdir)
        self.close_conn()
        self.corrupt_meta(newdir)
        copy_for_crash_restart(newdir, newdir2)

        for salvagedir in [ newdir, newdir2 ]:
            # Removing the 'WiredTiger.turtle' file has weird behavior:
            #  Immediately doing wiredtiger_open (without salvage) succeeds.
            #  Following that, wiredtiger_open w/ salvage also succeeds.
            #
            #  But, immediately after the corruption, if we run
            #  wiredtiger_open with salvage, it will fail.
            # This anomoly should be fixed or explained.
            if self.filename == 'WiredTiger.turtle':
                continue

            if self.is_salvageable():
                if self.filename == 'WiredTigerHS.wt':
                    # Without salvage, they result in an error during the wiredtiger_open.
                    # But the nature of the messages produced during the error is variable
                    # by which case it is, and even variable from system to system.
                    with self.expectedStdoutPattern('.'):
                        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                            lambda: self.reopen_conn(salvagedir, self.base_config),
                            '/.*/')

                self.reopen_conn(salvagedir, salvage_config)
                if self.filename == 'test_txn22':
                    self.checks()
            else:
                # Certain cases are not currently salvageable, they result in
                # an error during the wiredtiger_open.  But the nature of the
                # messages produced during the error is variable by which case
                # it is, and even variable from system to system.
                with self.expectedStdoutPattern('.'):
                    self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                        lambda: self.reopen_conn(salvagedir, salvage_config),
                        '/.*/')

if __name__ == '__main__':
    wttest.run()
