#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
# test_compat01.py
# Check compatibility API

import fnmatch, os
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet, simple_key
from wtscenario import make_scenarios

class test_compat01(wttest.WiredTigerTestCase, suite_subprocess):
    # Add enough entries and use a small log size to generate more than
    # one log file.
    entries = 2000
    logmax = "100K"
    tablename = 'test_compat01'
    uri = 'table:' + tablename
    # Declare the log versions that do and do not have prevlsn.
    # Log version 1 does not have the prevlsn record.
    # Log version 2 introduced that record.
    # Log version 3 continues to have that record.
    min_logv = 2

    # The API uses only the major and minor numbers but accepts with
    # and without the patch number.  Test both.
    start_compat = [
        ('def', dict(compat1='none', logv1=3)),
        ('31', dict(compat1="3.1", logv1=3)),
        ('31_patch', dict(compat1="3.1.0", logv1=3)),
        ('30', dict(compat1="3.0", logv1=2)),
        ('30_patch', dict(compat1="3.0.0", logv1=2)),
        ('26', dict(compat1="2.6", logv1=1)),
        ('26_patch', dict(compat1="2.6.1", logv1=1)),
        ('old', dict(compat1="1.8", logv1=1)),
        ('old_patch', dict(compat1="1.8.1", logv1=1)),
    ]
    restart_compat = [
        ('def2', dict(compat2='none', logv2=3)),
        ('31.2', dict(compat2="3.1", logv2=3)),
        ('31_patch2', dict(compat2="3.1.0", logv2=3)),
        ('30.2', dict(compat2="3.0", logv2=2)),
        ('30_patch2', dict(compat2="3.0.0", logv2=2)),
        ('26.2', dict(compat2="2.6", logv2=1)),
        ('26_patch2', dict(compat2="2.6.1", logv2=1)),
        ('old2', dict(compat2="1.8", logv2=1)),
        ('old_patch2', dict(compat2="1.8.1", logv2=1)),
    ]
    scenarios = make_scenarios(restart_compat, start_compat)

    def make_compat_str(self, create):
        compat_str = ''
        if (create == True and self.compat1 != 'none'):
            #compat_str = 'verbose=(temporary),compatibility=(release="%s"),' % self.compat1
            compat_str = 'compatibility=(release="%s"),' % self.compat1
        elif (create == False and self.compat2 != 'none'):
            #compat_str = 'verbose=(temporary),compatibility=(release="%s"),' % self.compat2
            compat_str = 'compatibility=(release="%s"),' % self.compat2
        return compat_str

    def conn_config(self):
        # Set archive false on the home directory.
        log_str = 'log=(archive=false,enabled,file_max=%s),' % self.logmax
        compat_str = self.make_compat_str(True)
        self.pr("Conn config:" + log_str + compat_str)
        return log_str + compat_str

    def check_prev_lsn(self, conn_close, prev_lsn_count):
        #
        # Run printlog and look for the prev_lsn log record.  Verify its
        # existence with the passed in expected result.  We don't use
        # check_file_contains because that only looks in the first 100K and
        # we don't know how big our text-based log output is.  Look through
        # the entire file if needed and set a boolean for comparison.
        #
        self.runWt(['printlog'], outfilename='printlog.out', closeconn=conn_close)
        pstr = str(prev_lsn_count)
        self.pr("CHECK PREV: Looking for " + pstr + " prev LSN entries")
        contains = 0
        with open('printlog.out') as logfile:
            for line in logfile:
                if 'optype' in line and 'prev_lsn' in line:
                    contains += 1
        self.assertEqual(prev_lsn_count, contains)

    def check_log(self, reconfig):
        orig_logs = fnmatch.filter(os.listdir('.'), "*gerLog*")
        compat_str = self.make_compat_str(False)
        if self.logv1 >= self.min_logv:
            prev_lsn_logs = len(orig_logs)
        else:
            prev_lsn_logs = 0
        pstr = str(prev_lsn_logs)
        self.pr("CHECK LOG: Orig " + pstr + " prev LSN log files")

        if not reconfig:
            #
            # Close and open the connection to force recovery and reset the
            # compatibility string on startup.
            #
            self.conn.close()
            log_str = 'log=(enabled,file_max=%s,archive=false),' % self.logmax
            restart_config = log_str + compat_str
            self.pr("Restart conn " + restart_config)
            #
            # Open a connection to force it to run recovery.
            #
            conn = self.wiredtiger_open('.', restart_config)
            conn.close()
            check_close = False
            #
            # If the version was upgraded we'll see a new log file containing
            # the new log record no matter what the original setting was.
            #
            if self.logv2 > 1:
                prev_lsn_logs += 1
        else:
            self.pr("Reconfigure: " + compat_str)
            self.conn.reconfigure(compat_str)
            check_close = True
            #
            # If we're reconfiguring, we'll see another new log file
            # when transitioning between log version numbers. Staying
            # at the same version has no effect. We'll only see another
            # new log file with the prevlsn if the new version supports it.
            #
            if self.logv1 != self.logv2 and self.logv2 >= self.min_logv:
                prev_lsn_logs += 1

        # Run printlog and verify the new record does or does not exist.
        # Need to check count of log files that should and should not have
        # the prev_lsn record based on the count of log files that exist
        # before and after.  Pass that into this function and check the
        # number of prev_lsn records we see.
        self.check_prev_lsn(check_close, prev_lsn_logs)

    def run_test(self, reconfig):
        # If reconfiguring with the empty string there is nothing to do.
        if reconfig == True and self.compat2 == 'none':
            return
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri, None)
        #
        # Add some entries to generate log files.
        #
        for i in range(self.entries):
            c[i] = i + 1
        c.close()

        # Check the log state after the entire op completes
        # and run recovery with the restart compatibility mode.
        self.check_log(reconfig)

    # Run the same test but reset the compatibility via
    # reconfigure or changing it when reopening the connection.
    def test_reconfig(self):
        self.run_test(True)

    def test_restart(self):
        self.run_test(False)

class test_reconfig_fail(wttest.WiredTigerTestCase):

    # Test an active transaction blocks upgrade/downgrade configuration.
    def test_reconfig_fail(self):
        uri = 'table:reconfig_fail'
        ds = SimpleDataSet(self, uri, 100, key_format='S')
        ds.populate()

        self.session.begin_transaction("isolation=snapshot")
        c = self.session.open_cursor(uri, None)
        c.set_key(ds.key(20))
        c.set_value("abcde")
        self.assertEquals(c.update(), 0)

        compat_str = 'compatibility=(release="3.0.0")'
        msg = '/system must be quiescent/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.conn.reconfigure(compat_str), msg)

if __name__ == '__main__':
    wttest.run()
