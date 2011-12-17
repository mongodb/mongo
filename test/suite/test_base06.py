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
        # Add a hint in case we're using mytruncate
        cursor._position_hint = ''
        if value >= self.nentries:
            cursor.last()
            cursor.next()
            cursor._position_hint = 'END'
        elif value < 0:
            cursor._position_hint = 'BEGIN'
        elif value >= 0:
            cursor.set_key(value)
            self.assertEqual(cursor.search(), 0)
            cursor._position_hint = str(value)
        return cursor

    def mytruncate(self, uri, begcursor, endcursor, notused):
        """
        A replacement for session.truncate() that can be used
        to test the testcase.
        """
        endkey = None
        if endcursor != None:
            if endcursor._position_hint != 'END':
                endkey = endcursor.get_key()
        cursor = begcursor
        done = False
        if cursor == None:
            cursor = self.session.open_cursor(uri, None, None)
            if cursor.next() == wiredtiger.WT_NOTFOUND:
                done = True
        removed = 0
        while not done and (endcursor == None or cursor.get_key() != endkey):
            cursor.remove()
            removed += 1
            if cursor.next() == wiredtiger.WT_NOTFOUND:
                done = True
        if begcursor == None:
            cursor.close()
        self.pr(uri + ' removed ' + str(removed))

    def truncateRangeAndCheck(self, tablename, begin, end):
        self.pr('truncateRangeAndCheck: ' + str(begin) + ',' + str(end))
        self.populate(self.table_name1)
        cur1 = self.setupCursor(tablename, begin)
        cur2 = self.setupCursor(tablename, end)
        beginerror = (begin != None and begin < 0)
        if beginerror:
            self.assertRaises(WiredTigerError, lambda:
                self.session.truncate('table:' + self.table_name1,
                                      cur1, cur2, None))
        else:
            self.session.truncate('table:' + self.table_name1, cur1, cur2, None)
        if cur1 != None:
            cur1.close()
        if cur2 != None:
            cur2.close()
        if begin == None:
            begin = 0
        if end == None:
            end = self.nentries

        cursor = self.session.open_cursor('table:' + tablename, None, None)
        self.pr('truncate(' + str(begin) + ' through ' + str(end) + ') => ' + \
            str([i for i in cursor]))
        cursor.close()

        cursor = self.session.open_cursor('table:' + tablename, None, None)
        want = 0
        # skip over numbers truncated
        if (not beginerror) and want >= begin and want < end:
            want = end
        for key,val in cursor:
            self.assertEqual(key, want)
            self.assertEqual(val, str(want))
            want += 1
            # skip over numbers truncated
            if (not beginerror) and want >= begin and want < end:
                want = end
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
        self.KNOWN_FAILURE('rename not supported')
        self.populate(self.table_name1)
        self.session.rename('table:' + self.table_name1,
                            'table:' + self.table_name2, None)
        self.checkContents(self.table_name2)
        self.checkDoesNotExist(self.table_name1)
        self.session.rename('table:' + self.table_name2,
                            'table:' + self.table_name1, None)
        self.checkContents(self.table_name1)
        self.checkDoesNotExist(self.table_name2)
        self.assertRaises(WiredTigerError, lambda:
            self.session.rename('table:' + self.table_name2,
                                'table:' + self.table_name1, None))

    def test_drop(self):
        self.populate(self.table_name1)
        self.session.drop('table:' + self.table_name1, None)
        self.checkDoesNotExist(self.table_name1)
        self.session.drop('table:' + self.table_name1, 'force')
        self.KNOWN_FAILURE('session drop of nonexistent file should fail')
        self.assertRaises(WiredTigerError, lambda:
            self.session.drop('table:' + self.table_name1, None))

    def test_truncate(self):
        self.populate(self.table_name1)

        # Note: to allow this test to proceed, even with a
        # non-functioning truncate, replace
        # self.session.truncate() with self.mytruncate().
        self.KNOWN_FAILURE('truncate not supported')
        self.session.truncate('table:' + self.table_name1, None, None, None)
        self.checkEmpty(self.table_name1)
        self.session.drop('table:' + self.table_name1, None)

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
        for begin in [None, -1, 0, 10]:
            for end in [None, self.nentries-1, self.nentries, self.nentries-10]:
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
