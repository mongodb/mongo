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
#
# test_readonly02.py
#   Readonly: Test readonly mode with illegal config combinations
#   and error checking during updates.
#

from helper import copy_wiredtiger_home
from suite_subprocess import suite_subprocess
import os, wiredtiger, wttest

class test_readonly02(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'table:test_readonly02'
    create = True
    create_params = 'key_format=i,value_format=i'
    entries = 10

    conn_params = \
        'create,' + \
        'log=(enabled,file_max=100K,zero_fill=true),' + \
        'operation_tracking=(enabled=false),'
    conn_params_rd = \
        'create,readonly=true,' + \
        'log=(enabled,zero_fill=false),' + \
        'operation_tracking=(enabled=false),'
    conn_params_rdcfg = \
        'create,readonly=true,log=(enabled),' + \
        'operation_tracking=(enabled=false),'

    #
    # Run to make sure incompatible configuration options return an error.
    # The situations that cause failures (instead of silent overrides) are:
    #   1. setting readonly on a new database directory
    #   2. an unclean shutdown and reopening readonly
    #   3. logging with zero-fill enabled and readonly
    #
    badcfg1 = 'log=(enabled,zero_fill=true)'

    def setUpConnectionOpen(self, dir):
        self.home = dir
        rdonlydir = dir + '.rdonly'
        #
        # First time through check readonly on a non-existent database.
        #
        if self.create:
            #   1. setting readonly on a new database directory
            # Setting readonly prevents creation so we should see an
            # error because the lock file does not exist.
            msg = '/No such file/'
            if os.name != 'posix':
                msg = '/cannot find the file/'
            os.mkdir(rdonlydir)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open(
                rdonlydir, self.conn_params_rd), msg)

        self.create = False
        conn = self.wiredtiger_open(dir, self.conn_params)
        return conn

    def check_unclean(self):
        backup = "WT_COPYDIR"
        copy_wiredtiger_home(self, self.home, backup, True)
        msg = '/needs recovery/'
        #   2. an unclean shutdown and reopening readonly
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open(backup, self.conn_params_rd), msg)

    def close_checkerror(self, cfg):
        ''' Close the connection and reopen readonly'''
        #
        # Close the original connection.  Reopen readonly and also with
        # the given configuration string.
        #
        self.close_conn()
        conn_params = self.conn_params_rd + cfg
        msg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open(self.home, conn_params), msg)

    def test_readonly(self):
        tablearg = self.tablename
        self.session.create(tablearg, self.create_params)
        c = self.session.open_cursor(tablearg, None, None)
        for i in range(self.entries):
            c[i+1] = i % 255
        # Check for an error on an unclean recovery/restart.
        self.check_unclean()

        # Close the connection.  Reopen readonly with other bad settings.
        #   3. logging with zero-fill enabled and readonly
        self.close_checkerror(self.badcfg1)

if __name__ == '__main__':
    wttest.run()
