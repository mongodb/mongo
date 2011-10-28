#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# suite_subprocess.py
# 	Run a subprocess within the test suite
#

import unittest
from wiredtiger import WiredTigerError
import wttest
import subprocess
import os

# Used as a 'mixin' class along with a WiredTigerTestCase class
class suite_subprocess:
    subproc = None

    def check_empty_file(self, filename):
        """
        Raise an error if the file is not empty
        """
        filesize = os.path.getsize(filename)
        if filesize > 0:
            with open(filename, 'r') as f:
                contents = f.read(1000)
                print 'ERROR: ' + filename + ' should be empty, but contains:\n'
                print contents + '...\n'
        self.assertEqual(filesize, 0)

    def runWt(self, args, outfilename=None, reopensession=True):
        """
        Run the 'wt' process
        """

        # we close the connection to guarantee everything is
        # flushed, and that we can open it from another process
        self.conn.close(None)
        self.conn = None

        wterrname = "wt.err"
        wtoutname = outfilename
        if wtoutname == None:
            wtoutname = "wt.out"
        with open(wterrname, "w") as wterr:
            with open(wtoutname, "w") as wtout:
                if self._gdbSubprocess:
                    procargs = ["gdb", "--args", "../../.libs/wt"]
                else:
                    procargs = ["../../wt"]
                procargs.extend(args)
                print "running: " + str(procargs)
                if self._gdbSubprocess:
                    print "*********************************************"
                    print "**** Run via: run " + " ".join(procargs[3:]) + ">" + wtoutname + " 2>" + wterrname
                    print "*********************************************"
                    proc = subprocess.Popen(procargs)
                else:
                    proc = subprocess.Popen(procargs, stdout=wtout, stderr=wterr)
                proc.wait()
        self.check_empty_file(wterrname)
        if outfilename == None:
            self.check_empty_file(wtoutname)

        # Reestablish the connection if needed
        if reopensession:
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)
