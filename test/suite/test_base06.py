#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base06.py
#	session level operations on tables
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest
import time
import os

class test_base06(wttest.WiredTigerTestCase):
    """
    Test various session level operations on tables, including
    rename, drop, sync, truncate

    """

    table_name1 = 'test_base06a'
    table_name2 = 'test_base06b'
    #nentries = 1000
    nentries = 30

    def populate(self, tablename):
        create_args = 'key_format=i,value_format=S'
        self.session.create("table:" + tablename, create_args)
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(i)
            cursor.set_value(str(i))
            cursor.insert()
        self.pr('populate: ' + tablename + ': added ' + str(self.nentries))
        cursor.close()

    def checkContents(self, tablename):
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        want = 0
        for key,val in cursor:
            self.assertEqual(key, want)
            self.assertEqual(val, str(want))
            want += 1
        self.assertEqual(want, self.nentries)
        cursor.close()

    def checkDoesNotExist(self, t):
        self.assertFalse(os.path.exists(t + ".wt"))
        self.assertRaises(WiredTigerError, lambda:
                              self.session.open_cursor('table:' + t,
                                                       None, None))

    def setupCursor(self, tablename, value):
        if value == None:
            return None
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        if value >= 0 and value < self.nentries:
            cursor.set_key(value)
            self.assertEqual(cursor.search(), 0)
        return cursor

    def truncateRangeAndCheck(self, tablename, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        self.populate(tablename)
        cur1 = self.setupCursor(tablename, begin)
        cur2 = self.setupCursor(tablename, end)
        beginerror = (begin != None and begin < 0)
        if not cur1 and not cur2:
            self.session.truncate('table:' + tablename, None, None, None)
        elif beginerror:
            self.assertRaises(WiredTigerError, lambda:
                self.session.truncate(None, cur1, cur2, None))
        else:
            self.session.truncate(None, cur1, cur2, None)
        if cur1:
            cur1.close()
        if cur2:
            cur2.close()
        if begin is None:
            begin = 0
        if end is None:
            end = self.nentries - 1

        cursor = self.session.open_cursor('table:' + tablename, None, None)
        self.pr('truncate(' + str(begin) + ' through ' + str(end) + ') => ' + \
            str([i for i in cursor]))
        cursor.close()

        cursor = self.session.open_cursor('table:' + tablename, None, None)
        want = 0
        # skip over numbers truncated
        if (not beginerror) and want >= begin and want <= end:
            want = end + 1
        for key,val in cursor:
            self.assertEqual(key, want)
            self.assertEqual(val, str(want))
            want += 1
            # skip over numbers truncated
            if want >= begin and want <= end:
                want = end + 1
        self.assertEqual(want, self.nentries)
        cursor.close()

    def checkEmpty(self, t):
        cursor = self.session.open_cursor('table:' + t, None, None)
        got = 0
        for key,val in cursor:
            got += 1
        self.assertEqual(got, 0)
        cursor.close()

    def test_nop(self):
        """ Make sure our test functions work """
        self.populate(self.table_name1)
        self.checkContents(self.table_name1)
        self.checkDoesNotExist(self.table_name2)

    def test_rename(self):
        self.populate(self.table_name1)
        self.session.rename('table:' + self.table_name1, self.table_name2, None)
        self.checkContents(self.table_name2)
        self.checkDoesNotExist(self.table_name1)
        self.session.rename('table:' + self.table_name2, self.table_name1, None)
        self.checkContents(self.table_name1)
        self.checkDoesNotExist(self.table_name2)
        self.assertRaises(WiredTigerError, lambda:
            self.session.rename(
                'table:' + self.table_name2, self.table_name1, None))

    def test_drop(self):
        self.populate(self.table_name1)
        self.session.drop('table:' + self.table_name1, None)
        self.checkDoesNotExist(self.table_name1)
        self.session.drop('table:' + self.table_name1, 'force')
        self.assertRaises(WiredTigerError, lambda:
            self.session.drop('table:' + self.table_name1, None))

    def test_truncate(self):
        self.populate(self.table_name1)

        self.session.truncate('table:' + self.table_name1, None, None, None)
        self.checkEmpty(self.table_name1)
        self.session.drop('table:' + self.table_name1, None)

    def test_truncate_cursor(self):
        # Test using cursors for the begin and end of truncate
        # For begin and end, the following applies:
        #   None means pass None for the cursor arg to truncate
        #   An integer N, with 0 <= N < self.nentries, passes
        #   a cursor positioned at that element.  0 positions
        #   a cursor before the first element, and self.nentries
        #   positions a cursor after the last element.
        #
        # We assume that the begin cursor must be positioned over
        # a value.  If not, we assume it raises an error.
        for begin in [None, 0, 10]:
            for end in [None, self.nentries - 1, self.nentries - 10]:
                self.truncateRangeAndCheck(self.table_name1, begin, end)
                self.session.drop('table:' + self.table_name1, None)
        self.truncateRangeAndCheck(self.table_name1, 10, 10)
        self.session.drop('table:' + self.table_name1, None)

    def test_sync(self):
        self.populate(self.table_name1)
        origmtime = os.path.getmtime(self.table_name1 + ".wt")
        time.sleep(2)
        self.session.sync('table:' + self.table_name1, None)
        newmtime = os.path.getmtime(self.table_name1 + ".wt")
        self.assertGreater(newmtime, origmtime)
        self.checkContents(self.table_name1)


if __name__ == '__main__':
    wttest.run()
