#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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
# test_sweep03.py
# Test to confirm if setting close_idle_time to 0 does not sweep old handles
#

import fnmatch, os, shutil, run, time
from suite_subprocess import suite_subprocess
from wiredtiger import wiredtiger_open, stat
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_sweep03(wttest.WiredTigerTestCase, suite_subprocess):
    tablebase = 'test_sweep03'
    uri = 'table:' + tablebase
    numfiles = 50
    numkv = 1000
    ckpt = 5

    types = [
        ('row', dict(tabletype='row',
                    create_params = 'key_format=i,value_format=i')),
    ]

    scenarios = types

    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        self.home = dir
        self.backup_dir = os.path.join(self.home, "WT_BACKUP")

        conn_params = \
                ',create,error_prefix="%s: ",' % self.shortid() + \
                'file_manager=(close_handle_minimum=0,' + \
                'close_idle_time=0,close_scan_interval=2),' + \
                'checkpoint=(wait=%d),' % self.ckpt + \
                'statistics=(fast),'
        # print "Creating conn at '%s' with config '%s'" % (dir, conn_params)
        try:
            conn = wiredtiger_open(dir, conn_params)
        except wiredtiger.WiredTigerError as e:
            print "Failed conn at '%s' with config '%s'" % (dir, conn_params)
        self.pr(`conn`)
        self.session2 = conn.open_session()
        return conn

    def test_ops(self):
        #
        # Set up numfiles with numkv entries.  We just want some data in there
        # we don't care what it is.
        #
        for f in range(self.numfiles):
            uri = '%s.%d' % (self.uri, f)
            # print "Creating %s with config '%s'" % (uri, self.create_params)
            self.session.create(uri, self.create_params)
            c = self.session.open_cursor(uri, None)
            for k in range(self.numkv):
                c[k+1] = 1
            c.close()
            if f % 20 == 0:
                time.sleep(1)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close1 = stat_cursor[stat.conn.dh_conn_handles][2]
        sweep1 = stat_cursor[stat.conn.dh_conn_sweeps][2]
        stat_cursor.close()

        #
        # We've configured checkpoints to run every 5 seconds, sweep server to
        # run every 2 seconds and idle time to 0, disabled. It should take
        # about 2~ seconds for a handle to be closed assuming any are.
        # Sleep for 12 seconds to be safe.
        #
        uri = '%s.test' % self.uri
        self.session.create(uri, self.create_params)

        #
        # Keep inserting data to keep at least one handle active and give
        # checkpoint something to do.  Make sure checkpoint doesn't adjust
        # the time of death for inactive handles.
        #
        c = self.session.open_cursor(uri, None)
        k = 0
        sleep = 0
        while sleep < 12:
            k = k+1
            c[k] = 1
            sleep += 2
            time.sleep(2)
        c.close()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close2 = stat_cursor[stat.conn.dh_conn_handles][2]
        sweep2 = stat_cursor[stat.conn.dh_conn_sweeps][2]
        stat_cursor.close()

        # We expect nothing to have been closed, so dh_conn_handles should be 0
        if (close1 != 0 or close2 != 0):
            print "close1: " + str(close1) + " close2: " + str(close2)
            print "XX: sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
        self.assertEqual(close1, 0)
        self.assertEqual(close2, 0)

        # Create a table to drop. A drop should close its associated handles
        drop_uri = '%s.%s' % (self.uri, "droptest")

        self.session.create(drop_uri, self.create_params)

        c = self.session.open_cursor(drop_uri, None)
        for k in range(self.numkv):
            c[k+1] = 1
        c.close()

        self.session.drop(drop_uri, "force=true")

        time.sleep(12)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close3 = stat_cursor[stat.conn.dh_conn_handles][2]
        sweep3 = stat_cursor[stat.conn.dh_conn_sweeps][2]
        stat_cursor.close()

        # We dropped the table, something should have been cleaned up
        if (close3 != 1):
            print "close3: " + str(close3) + " close2: " + str(close2)
            print "XX: sweep3: " + str(sweep3) + " sweep2: " + str(sweep2)
        self.assertEqual(close3, 1)

if __name__ == '__main__':
    wttest.run()
