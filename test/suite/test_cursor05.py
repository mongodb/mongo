#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# test_cursor05.py
# 	Test cursors at the point where a cursor is first
#	initialized, and when it hits an endpoint.
#	Mix that in with column groups.
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class test_cursor05(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    nentries = 2

    def populate(self, count):
        """ Populate the given number of entries. """
        cursor = self.session.open_cursor('table:main', None, None)
        for i in range(0, count):
            cursor.set_key(i, 'key' + str(i))
            cursor.set_value('val' + str(i), i, 'val' + str(i), i)
            cursor.insert()
        cursor.close()

    def check_iterate_forward(self, cursor, expectcount):
        """ Use the cursor to iterate and check for the expected entries. """
        i = 0
        for ikey, skey, s1, i2, s3, i4 in cursor:
            print 'forward: ' + str([ikey, skey, s1, i2, s3, i4])
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(i))
            self.assertEqual(i2, i)
            self.assertEqual(s3, 'val' + str(i))
            self.assertEqual(i4, i)
            i += 1
        self.assertEqual(i, expectcount)

    def check_iterate_backward(self, cursor, expectcount):
        """ Iterate backwards and check for the expected entries. """
        i = expectcount
        while cursor.prev() == 0:
            i -= 1
            (ikey, skey) = cursor.get_keys()
            (s1, i2, s3, i4) = cursor.get_values()
            print 'backward: ' + str([ikey, skey, s1, i2, s3, i4])
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(i))
            self.assertEqual(i2, i)
            self.assertEqual(s3, 'val' + str(i))
            self.assertEqual(i4, i)
        self.assertEqual(i, 0)

    def check_iterate(self, cursor, expectcount, isforward):
        """
        Use the cursor to iterate (forwards or backwards)
        and check for the expected entries.
        """
        if isforward:
            self.check_iterate_forward(cursor, expectcount)
        else:
            self.check_iterate_backward(cursor, expectcount)

    def check_entries(self, testmode, expectcount, isforward):
        """
        Use various modes to get the cursor to the 'uninitialized' state,
        and verify that is correct by iterating and checking each element.
        """
        cursor = self.session.open_cursor('table:main', None, None)

        # The cursor is uninitialized.  Any of these sequences should
        # leave the cursor uninitialized again - ready to iterate.
        if testmode == 0:
            pass
        elif testmode == 1:
            cursor.next()
            cursor.prev()
        elif testmode == 2:
            cursor.prev()
            cursor.next()

        # Verify that by iterating
        self.check_iterate(cursor, expectcount, isforward)

        # Do something that leaves the cursor in an uninitialized spot
        if expectcount > 0:
            n = expectcount - 1
            cursor.set_key(n, 'key' + str(n))
            cursor.search()
            (s1, i2, s3, i4) = cursor.get_values()
            self.assertEqual(s1, 'val' + str(n))
            self.assertEqual(i2, n)
            self.assertEqual(s3, 'val' + str(n))
            self.assertEqual(i4, n)

        # Any of these should leave the cursor again positioned at
        # an uninitialized spot - ready to iterate
        if testmode == 0:
            cursor.reset()
        elif testmode == 1:
            cursor.reset()
            cursor.next()
            cursor.prev()
        elif testmode == 2:
            cursor.reset()
            cursor.prev()
            cursor.next()

        # Verify that by iterating
        self.check_iterate(cursor, expectcount, isforward)

        # After an iteration is complete, the cursor should be in
        # the same state as after reset(), or when first created.
        if testmode == 0:
            pass
        elif testmode == 1:
            cursor.next()
            cursor.prev()
        elif testmode == 2:
            cursor.prev()
            cursor.next()

        # Verify that by iterating
        self.check_iterate(cursor, expectcount, isforward)

        cursor.close()

    def common_test(self, nentries, hascolgroups):
        cgstr = ',colgroups=(c1,c2)' if hascolgroups else ''
        self.session.create('table:main', 'key_format=iS,value_format=SiSi,'
                            'columns=(ikey,Skey,S1,i2,S3,i4)' + cgstr)
        if hascolgroups:
            self.session.create("colgroup:main:c1", "columns=(S1,i2)")
            self.session.create("colgroup:main:c2", "columns=(S3,i4)")
        self.populate(nentries)
        self.check_entries(0, nentries, True)
        self.check_entries(1, nentries, True)
        self.check_entries(2, nentries, True)
        self.check_entries(0, nentries, False)
        self.check_entries(1, nentries, False)
        self.check_entries(2, nentries, False)

    def test_without_colgroups(self):
        self.common_test(3, False)

    def test_with_colgroups(self):
        self.common_test(3, True)

    def test_empty_without_colgroups(self):
        self.common_test(0, False)

    def test_empty_with_colgroups(self):
        self.common_test(0, True)

if __name__ == '__main__':
    wttest.run()
