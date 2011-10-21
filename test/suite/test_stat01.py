#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_stat01.py
# 	Statistics operations
#

import unittest
import wiredtiger
import wttest
import test_base03

class test_stat01(wttest.WiredTigerTestCase):
    """
    Test statistics
    """

    tablename = 'test_stat01.wt'
    nentries = 25

    def check_stats(self, statcursor, mincount, lookfor):
        """
        Do a quick check of the entries in the the stats cursor,
        There should be at least 'mincount' entries,
        and the 'lookfor' string should appear
        """
        stringclass = ''.__class__
        intclass = (0).__class__
        # make sure statistics basically look right
        count = 0
        found = False
        for key in statcursor:
            self.assertEqual(3, len(key))
            self.assertEqual(type(key[0]), stringclass)
            self.assertEqual(type(key[1]), stringclass)
            self.assertEqual(type(key[2]), intclass)
            print '  stat: ' + str(key)
            count += 1
            if key[0] == lookfor:
                found = True
        self.assertTrue(count > mincount)
        self.assertTrue(found, 'in stats, did not see: ' + lookfor)

    def test_statistics(self):
        self.skipTest('TODO: statistics need fixing before enabling this test')
        extra_params = ',allocation_size=512,internal_node_max=16384,leaf_node_max=131072'
        self.session.create('table:' + self.tablename, 'key_format=S,value_format=S' + extra_params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        value = ""
        for i in range(0, self.nentries):
            key = str(i)
            value = value + key + value # size grows exponentially
            cursor.set_key(key)
            cursor.set_value(value)
            cursor.insert()
        cursor.close()

        print 'overall stats:'
        allstat_cursor = self.session.open_cursor('statistics:', None, None)
        self.check_stats(allstat_cursor, 10, 'blocks written to a file')
        allstat_cursor.close()

        print 'file specific stats:'
        filestat_cursor = self.session.open_cursor('statistics:file:' + self.tablename, None, None)
        self.check_stats(filestat_cursor, 10, 'blocks written to a file')
        filestat_cursor.close()

        no_cursor = self.session.open_cursor('statistics:file:DoesNotExist', None, None)
        self.assertEqual(no_cursor, None)

if __name__ == '__main__':
    wttest.run()
