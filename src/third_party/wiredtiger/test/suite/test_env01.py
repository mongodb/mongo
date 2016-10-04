#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os
import wiredtiger, wttest

# test_priv01.py
#    Test privileged operations.
#    This is a variant of test_config02.py.  This test should be run as both
# normal and privileged (e.g. root) user, and should pass in both cases.
class test_priv01(wttest.WiredTigerTestCase):
    """
    This tests privileged operations.
    The only part of WiredTiger that currently cares about privilege
    levels is the 'use_environment_priv' configuration option for
    wiredtiger_open, which by design, behaves differently depending on
    whether the current process is running as a privileged user or
    not.  This test should be run as both normal and privileged
    (e.g. root) user to fully test both cases.
    """
    table_name1 = 'test_priv01'
    nentries = 100

    def setUp(self):
        if os.name == 'nt':
            self.skipTest('Unix specific test skipped on Windows')
        super(test_priv01, self).setUp()

    # Each test needs to set up its connection in its own way,
    # so override these methods to do nothing
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def populate_and_check(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S'
        self.session.create("table:" + self.table_name1, create_args)
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor[str(1000000 + i)] = 'value' + str(i)
        i = 0
        cursor.reset()
        for key, value in cursor:
            self.assertEqual(key, str(1000000 + i))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def checkfiles(self, dirname):
        self.assertTrue(os.path.exists(dirname + os.sep + self.table_name1 + ".wt"))

    def checknofiles(self, dirname):
        nfiles = len([nm for nm in os.listdir(dirname) if os.path.isfile(nm)])
        self.assertEqual(nfiles, 0)

    def common_test(self, homearg, homeenv, configextra):
        """
        Call wiredtiger_open and run a simple test.
        homearg is the first arg to wiredtiger_open, it may be null.
        WIREDTIGER_HOME is set to homeenv, if it is not null.
        configextra are any extra configuration strings needed on the open.
        """
        configarg = 'create'
        if configextra != None:
            configarg += ',' + configextra
        if homeenv == None:
            os.unsetenv('WIREDTIGER_HOME')
        else:
            os.putenv('WIREDTIGER_HOME', homeenv)
        try:
            self.conn = self.wiredtiger_open(homearg, configarg)
            self.session = self.conn.open_session(None)
            self.populate_and_check()
        finally:
            os.unsetenv('WIREDTIGER_HOME')

    def test_home_and_env_conf_priv(self):
        # If homedir is set, the environment is ignored
        hdir = 'homedir'
        edir = 'envdir'
        os.mkdir(hdir)
        os.mkdir(edir)
        self.common_test(hdir, edir, 'use_environment_priv=true')
        self.checkfiles(hdir)
        self.checknofiles(edir)

    def test_home_and_missing_env_priv(self):
        # If homedir is set, it is used no matter what
        hdir = 'homedir'
        os.mkdir(hdir)
        self.common_test(hdir, None, 'use_environment_priv=true')
        self.checkfiles(hdir)

    def test_env_conf_nopriv(self):
        edir = 'envdir'
        os.mkdir(edir)
        if os.getuid() != os.geteuid():
            print 'Running ' + str(self) + ' as privileged user'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.common_test(None, edir, None),
                '/WIREDTIGER_HOME environment variable set but\
 process lacks privileges to use that environment variable/')

    def test_env_conf_priv(self):
        edir = 'envdir'
        os.mkdir(edir)
        privarg = 'use_environment_priv=true'
        if os.getuid() != os.geteuid():
            print 'Running ' + str(self) + ' as privileged user'
            self.common_test(None, edir, privarg)
            self.checkfiles(edir)
            self.checknofiles(".")

    def test_env_conf_without_env_var_priv(self):
        # no env var set, so should use current directory
        self.common_test(None, None, 'use_environment_priv=true')
        self.checkfiles(".")

    def test_env_conf(self):
        # If no homedir is set, use environment
        edir = 'envdir'
        os.mkdir(edir)
        self.common_test(None, edir, 'use_environment=true')
        # this check fails because of test harness files:
        #self.checknofiles('.')
        self.checkfiles(edir)

    def test_env_conf_off(self):
        # If no homedir is set, use environment
        edir = 'envdir'
        os.mkdir(edir)
        self.common_test(None, edir, 'use_environment=false')
        self.checknofiles(edir)
        self.checkfiles('.')

if __name__ == '__main__':
    wttest.run()
