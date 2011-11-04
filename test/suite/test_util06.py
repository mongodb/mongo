#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util06.py
# 	Utilities: wt salvage
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util06(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util06.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'
    unique = 'SomeUniqueString'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        key = ''
        for i in range(0, self.nentries):
            key += str(i)
            if i == self.nentries / 2:
                val = self.unique + '0'
            else:
                val = key + key
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()
        cursor.close()

    def check_populate(self, tablename):
        """
        Verify that items added by populate are still there
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        wantkey = ''
        i = 0
        for gotkey, gotval in cursor:
            wantkey += str(i)
            if i == self.nentries / 2:
                wantval = self.unique + '0'
                wantval2 = self.unique + '1'
            else:
                wantval = wantkey + wantkey
                wantval2 = 'DoNotMatch'
            self.assertTrue(gotkey == wantkey or gotkey == wantkey2)
            self.assertEqual(gotval, wantval)
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def damage(self, tablename):
        """
        Open the file for the table, find the unique string
        and modify it.
        """
        self.close_conn()
        # we close the connection to guarantee everything is
        # flushed and closed from the WT point of view.
        filename = tablename + ".wt"

        fp = open(filename, "r+b")
        matchpos = 0
        match = self.unique
        matchlen = len(match)
        c = fp.read(1)
        while c and matchpos < matchlen:
            if match[matchpos] == c:
                matchpos += 1
            else:
                matchpos = 0
            c = fp.read(1)
        # Make sure we found the embedded string
        self.assertEqual(matchlen, matchpos)
        # We're already positioned, so alter it
        fp.write('1')
        fp.close()

    def test_salvage_process_empty(self):
        """
        Test salvage in a 'wt' process, using an empty table
        """
        self.skipTest('TODO: salvage does not work on an empty table...')
        self.session.create('table:' + self.tablename, self.session_params)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)

    def test_salvage_process(self):
        """
        Test salvage in a 'wt' process, using a populated table.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)
        self.check_populate(self.tablename)

    def test_salvage_api_empty(self):
        """
        Test salvage via API, using an empty table
        """
        self.skipTest('TODO: salvage does not work on an empty table...')
        self.session.create('table:' + self.tablename, self.session_params)
        self.session.salvage('table:' + self.tablename, None)

    def test_salvage_api(self):
        """
        Test salvage via API, using a populated table.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.session.salvage('file:' + self.tablename + ".wt", None)
        self.check_populate(self.tablename)

    def test_salvage_api_open_handle(self):
        """
        Test salvage via API, with an open connection/session.
        It should raise an exception.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)

        # damage() closed the session/connection, reopen them now.
        self.open_conn()
        self.session.salvage('file:' + self.tablename + ".wt")

    def test_salvage_api_damaged(self):  #TODO
        """
        Test salvage via API, on a damaged table.
        """
        self.skipTest('TODO: salvage cannot recover from this damage, why not?')
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)

        # damage() closed the session/connection, reopen them now.
        self.open_conn()
        self.assertRaises(WiredTigerError, lambda: self.session.verify('table:' + self.tablename, None))

        # and close, since that's required for salvage
        self.session.salvage('file:' + self.tablename + ".wt", None)

    def test_salvage_process_damaged(self):
        """
        Test salvage in a 'wt' process on a table that is purposely damaged.
        """
        self.skipTest('TODO: salvage does not work on this.')
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_non_empty_file(errfile)  # expect some output
        self.check_no_error_in_file(errfile)

if __name__ == '__main__':
    wttest.run()
