#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#   All rights reserved.
#
# test001.py
#   Basic operations
#

import unittest
import wiredtiger
import wttest
import os
import sys

class test_compress01_base(wttest.WiredTigerTestCase):
    """
    Test basic compression
    """
    nrecords = 10000

    # running tests from the base class uses no compressor,
    # a reasonable way to test the test case.
    def __init__(self, testname, compressor_name=None, abbrev='none'):
        wttest.WiredTigerTestCase.__init__(self, testname)
        self.compressor_name = compressor_name
        self.table_name1 = 'test_compress01' + abbrev + '.wt'
        # bigvalue = '1234567891011121314....'
        self.bigvalue = ''.join([`num` for num in xrange(1,10000)])  # about 38K chars long

    def create_table(self, tablename):
        extra_params = ',internal_node_min=512,internal_node_max=16384,leaf_node_min=131072,leaf_node_max=131072'
        comp_params = ''
        if self.compressor_name != None:
            comp_params = ',block_compressor=' + self.compressor_name
        params = 'key_format=S,value_format=S' + extra_params + comp_params
        self.pr('create_table: ' + tablename + ', params: ' + params)
        self.session.create('file:' + tablename, params)

    def cursor_s(self, tablename, key):
        cursor = self.session.open_cursor('file:' + tablename, None)
        cursor.set_key(key)
        return cursor

    def cursor_ss(self, tablename, key, val):
        cursor = self.cursor_s(tablename, key)
        cursor.set_value(val)
        return cursor

    def record_count(self):
        return self.nrecords

    def do_insert(self):
        """
        Create a table, add keys with big values, get them back
        """
        self.create_table(self.table_name1)

        self.pr("inserting `len(self.bigvalue)` byte values")
        for idx in xrange(1,self.record_count()):
            val = `idx` + self.bigvalue + `idx`
            inscursor = self.cursor_ss(self.table_name1, 'key' + `idx`, val)
            inscursor.insert()
            inscursor.close

    def do_verify(self):
        self.pr('search')
        for idx in xrange(1,self.record_count()):
            val = `idx` + self.bigvalue + `idx`
            getcursor = self.cursor_s(self.table_name1, 'key' + `idx`)
            ret = getcursor.search()
            self.assertTrue(ret == 0)
            self.assertEquals(getcursor.get_value(), val)
            getcursor.close(None)

    def do_fresh_cache(self):
        # Since we are running WT in-process, we just need
        # to shut down the connection and start again.
        self.conn.close(None)
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)

    def test_insert_and_verify(self):
        self.do_insert()
        # We want a fresh cache so that compressed pages
        # are really read from disk. 
        self.do_fresh_cache()
        self.do_verify()

    def extensionArg(self, name):
        if name != None:
            testdir = os.path.dirname(__file__)
            import run
            extdir = os.path.join(run.wt_builddir, 'ext/compressors')
            extfile = os.path.join(extdir, name, '.libs', name + '.so')
            if not os.path.exists(extfile):
                self.skipTest('Extension "' + extfile + '" not built')
            return 'extensions=["' + extfile + '"]'
        else:
            return ''

    # override WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        return self.setUpConnectionWithExtension(dir, self.compressor_name)
        
    def setUpConnectionWithExtension(self, dir, name):
        conn = wiredtiger.wiredtiger_open(dir, 'create,' + self.extensionArg(name))
        self.pr(`conn`)
        return conn


class test_compress01_1_nop(test_compress01_base):
    def __init__(self, testname):
        test_compress01_base.__init__(self, testname, 'nop_compress', 'nop')


class test_compress01_2_bz(test_compress01_base):
    def __init__(self, testname):
        test_compress01_base.__init__(self, testname, 'bzip2_compress', 'bz')


if __name__ == '__main__':
    wttest.run(test_compress01_base)
    wttest.run(test_compress01_1_nop)
    wttest.run(test_compress01_2_bz)
