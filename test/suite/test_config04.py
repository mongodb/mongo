#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# test_config04.py
# 	Individually test config options
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest
import os

class test_config04(wttest.WiredTigerTestCase):
    table_name1 = 'test_config04'
    nentries = 100

    K = 1024
    M = K * K
    G = K * M
    T = K * G

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
            cursor.set_key(str(1000000 + i))
            cursor.set_value('value' + str(i))
            cursor.insert()
        i = 0
        cursor.reset()
        for key, value in cursor:
            self.assertEqual(key, str(1000000 + i))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close(None)

    def common_test(self, configextra):
        """
        Call wiredtiger_open and run a simple test.
        configextra are any extra configuration strings needed on the open.
        """
        configarg = 'create'
        if configextra != None:
            configarg += ',' + configextra
        self.conn = wiredtiger.wiredtiger_open('.', configarg)
        self.session = self.conn.open_session(None)
        self.populate_and_check()

    def common_cache_size_test(self, sizestr, size):
        self.common_test('cache_size=' + sizestr)
        cursor = self.session.open_cursor('statistics:', None, None)
        cursor.set_key(wiredtiger.stat.cache_bytes_max)
        self.assertEqual(cursor.search(), 0)
        got_cache = cursor.get_values()[2]
        self.assertEqual(got_cache, size)

    def test_bad_config(self):
        self.assertRaises(WiredTigerError, lambda:
            wiredtiger.wiredtiger_open('.', 'not_valid,another_bad=10'))

    def test_cache_size_number(self):
        # Use a number without multipliers
        # 1M is the minimum, we'll ask for 1025 * 1024
        cache_size_str = str(1025 * 1024)
        self.common_cache_size_test(cache_size_str, 1025*self.K)

    def test_cache_size_K(self):
        # Kilobyte sizing test
        # 1M is the minimum, so ask for that using K notation.
        self.common_cache_size_test('1024K', 1024*self.K)

    def test_cache_size_M(self):
        # Megabyte sizing test
        self.common_cache_size_test('30M', 30*self.M)

    def test_cache_size_G(self):
        # Gigabyte sizing test
        # We are specifying the maximum the cache can grow,
        # not the initial cache amount, so small tests like
        # this can still run on smaller machines.
        self.common_cache_size_test('7G', 7*self.G)

    def test_cache_size_T(self):
        # Terabyte sizing test
        # We are specifying the maximum the cache can grow,
        # not the initial cache amount, so small tests like
        # this can still run on smaller machines.
        self.common_cache_size_test('2T', 2*self.T)

    def test_cache_too_small(self):
        self.assertRaises(WiredTigerError, lambda:
            wiredtiger.wiredtiger_open('.', 'create,cache_size=900000'))

    def test_cache_too_large(self):
        T11 = 11 * self.T  # 11 Terabytes
        self.assertRaises(WiredTigerError, lambda:
            wiredtiger.wiredtiger_open('.', 'create,cache_size=' + str(T11)))

    def test_eviction(self):
        self.common_test('eviction_target=84,eviction_trigger=94')
        # TODO: how do we verify that it was set?

    def test_eviction_bad(self):
        self.assertRaises(WiredTigerError, lambda:
            wiredtiger.wiredtiger_open('.', 'create,eviction_target=91,' +
                                       'eviction_trigger=81'))

    def test_eviction_bad2(self):
        self.assertRaises(WiredTigerError, lambda:
            wiredtiger.wiredtiger_open('.', 'create,eviction_target=86,' +
                                       'eviction_trigger=86'))

    def test_exclusive(self):
        self.common_test('exclusive')
        # TODO: how do we verify that it was set?

    def test_hazard_max(self):
        self.common_test('hazard_max=50')
        # TODO: how do we verify that it was set?

    def test_session_max(self):
        self.common_test('session_max=99')
        # TODO: how do we verify that it was set?

    def test_multiprocess(self):
        self.common_test('multiprocess')
        # TODO: how do we verify that it was set?

    def test_error_prefix(self):
        self.common_test('error_prefix="MyOwnPrefix"')
        # TODO: how do we verify that it was set?

    def test_logging(self):
        self.common_test('logging')
        # TODO: how do we verify that it was set?

    def test_transactional(self):
        self.common_test('transactional')
        # TODO: how do we verify that it was set?

if __name__ == '__main__':
    wttest.run()
