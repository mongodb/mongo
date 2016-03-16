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
#
# test_sweep01.py
# Test lots of tables, number of open files and sweeping.  Run both
# with and without checkpoints.
#

import fnmatch, os, shutil, run, time
from suite_subprocess import suite_subprocess
from wiredtiger import stat
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_sweep01(wttest.WiredTigerTestCase, suite_subprocess):
    tablebase = 'test_sweep01'
    uri = 'table:' + tablebase
    numfiles = 30
    numkv = 1000
    conn_config = 'file_manager=(close_handle_minimum=0,' + \
                  'close_idle_time=6,close_scan_interval=2),' + \
                  'statistics=(fast),'

    types = [
        ('row', dict(tabletype='row',
                    create_params = 'key_format=i,value_format=i')),
        ('var', dict(tabletype='var',
                    create_params = 'key_format=r,value_format=i')),
        ('fix', dict(tabletype='fix',
                    create_params = 'key_format=r,value_format=8t')),
    ]

    scenarios = types

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
        close1 = stat_cursor[stat.conn.dh_sweep_close][2]
        remove1 = stat_cursor[stat.conn.dh_sweep_remove][2]
        sweep1 = stat_cursor[stat.conn.dh_sweeps][2]
        sclose1 = stat_cursor[stat.conn.dh_session_handles][2]
        ssweep1 = stat_cursor[stat.conn.dh_session_sweeps][2]
        tod1 = stat_cursor[stat.conn.dh_sweep_tod][2]
        ref1 = stat_cursor[stat.conn.dh_sweep_ref][2]
        nfile1 = stat_cursor[stat.conn.file_open][2]
        stat_cursor.close()

        #
        # We've configured checkpoints to run every 5 seconds, sweep server to
        # run every 2 seconds and idle time to be 6 seconds. It should take
        # about 8 seconds for a handle to be closed. Sleep for double to be
        # safe.
        #
        uri = '%s.test' % self.uri
        self.session.create(uri, self.create_params)

        #
        # Keep inserting data to keep at least one handle active and give
        # checkpoint something to do.  Make sure checkpoint doesn't adjust
        # the time of death for inactive handles.
        #
        # Note that we do checkpoints inline because that has the side effect
        # of sweeping the session cache, which will allow handles to be
        # removed.
        #
        c = self.session.open_cursor(uri, None)
        k = 0
        sleep = 0
        max = 60
        final_nfile = 4
        while sleep < max:
            self.session.checkpoint()
            k = k+1
            c[k] = 1
            sleep += 2
            time.sleep(2)
            # Give slow machines time to process files.
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            this_nfile = stat_cursor[stat.conn.file_open][2]
            stat_cursor.close()
            self.pr("==== loop " + str(sleep))
            self.pr("this_nfile " + str(this_nfile))
            if this_nfile == final_nfile:
                break
        c.close()
        self.pr("Sweep loop took " + str(sleep))

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close2 = stat_cursor[stat.conn.dh_sweep_close][2]
        remove2 = stat_cursor[stat.conn.dh_sweep_remove][2]
        sweep2 = stat_cursor[stat.conn.dh_sweeps][2]
        sclose2 = stat_cursor[stat.conn.dh_session_handles][2]
        ssweep2 = stat_cursor[stat.conn.dh_session_sweeps][2]
        nfile2 = stat_cursor[stat.conn.file_open][2]
        tod2 = stat_cursor[stat.conn.dh_sweep_tod][2]
        ref2 = stat_cursor[stat.conn.dh_sweep_ref][2]
        stat_cursor.close()
        # print "checkpoint: " + str(self.ckpt)
        # print "nfile1: " + str(nfile1) + " nfile2: " + str(nfile2)
        # print "close1: " + str(close1) + " close2: " + str(close2)
        # print "sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
        # print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
        # print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
        # print "tod1: " + str(tod1) + " tod2: " + str(tod2)
        # print "ref1: " + str(ref1) + " ref2: " + str(ref2)

        #
        # The files are all closed.  Check that sweep did its work even
        # in the presence of recent checkpoints.
        #
        if (close1 >= close2):
            print "XX: close1: " + str(close1) + " close2: " + str(close2)
            print "remove1: " + str(remove1) + " remove2: " + str(remove2)
            print "sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
            print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
            print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
            print "tod1: " + str(tod1) + " tod2: " + str(tod2)
            print "ref1: " + str(ref1) + " ref2: " + str(ref2)
            print "nfile1: " + str(nfile1) + " nfile2: " + str(nfile2)
        self.assertEqual(close1 < close2, True)
        if (remove1 >= remove2):
            print "close1: " + str(close1) + " close2: " + str(close2)
            print "XX: remove1: " + str(remove1) + " remove2: " + str(remove2)
            print "sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
            print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
            print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
            print "tod1: " + str(tod1) + " tod2: " + str(tod2)
            print "ref1: " + str(ref1) + " ref2: " + str(ref2)
            print "nfile1: " + str(nfile1) + " nfile2: " + str(nfile2)
        self.assertEqual(remove1 < remove2, True)
        if (sweep1 >= sweep2):
            print "close1: " + str(close1) + " close2: " + str(close2)
            print "remove1: " + str(remove1) + " remove2: " + str(remove2)
            print "XX: sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
            print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
            print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
            print "tod1: " + str(tod1) + " tod2: " + str(tod2)
            print "ref1: " + str(ref1) + " ref2: " + str(ref2)
        self.assertEqual(sweep1 < sweep2, True)
        if (nfile2 >= nfile1):
            print "close1: " + str(close1) + " close2: " + str(close2)
            print "remove1: " + str(remove1) + " remove2: " + str(remove2)
            print "sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
            print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
            print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
            print "tod1: " + str(tod1) + " tod2: " + str(tod2)
            print "ref1: " + str(ref1) + " ref2: " + str(ref2)
            print "XX: nfile1: " + str(nfile1) + " nfile2: " + str(nfile2)
        self.assertEqual(nfile2 < nfile1, True)
        # The only files that should be left are the metadata, the lookaside
        # file, the lock file, and the active file.
        if (nfile2 != final_nfile):
            print "close1: " + str(close1) + " close2: " + str(close2)
            print "remove1: " + str(remove1) + " remove2: " + str(remove2)
            print "sweep1: " + str(sweep1) + " sweep2: " + str(sweep2)
            print "sclose1: " + str(sclose1) + " sclose2: " + str(sclose2)
            print "ssweep1: " + str(ssweep1) + " ssweep2: " + str(ssweep2)
            print "tod1: " + str(tod1) + " tod2: " + str(tod2)
            print "ref1: " + str(ref1) + " ref2: " + str(ref2)
            print "XX2: nfile1: " + str(nfile1) + " nfile2: " + str(nfile2)
        self.assertEqual(nfile2 == final_nfile, True)

if __name__ == '__main__':
    wttest.run()
