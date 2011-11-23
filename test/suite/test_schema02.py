#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_schema02.py
# 	Columns, column groups, indexes
#

import unittest
import wiredtiger
from wiredtiger import WiredTigerError
import wttest

class test_schema02(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    nentries = 1000

    def expect_failure_primary(self, configstr):
        self.assertRaises(WiredTigerError,
                          lambda:self.session.create("table:main", configstr))

    def expect_failure_colgroup(self, name, configstr):
        self.assertRaises(WiredTigerError,
                          lambda:self.session.create("colgroup:" + name, configstr))

    def test_colgroup_after_failure(self):
        # Remove this line to see the failure.
        # If the expect_failure_primary() line is removed,
        # then the test succeeds.  With that line, the colgroup
        # dies with 'Invalid argument'. Somehow the first failure seems
        # to poison table:main.
        self.KNOWN_FAILURE('creating colgroup after a previous failure')

        # bogus formats
        self.expect_failure_primary("key_format=Z,value_format=Y")

        # These should succeed
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

    def test_colgroup_failures(self):
        # Remove this line to see the failure.
        # There are multiple errors, see '#### TEST FAILURE HERE ####'
        # below for where the test is known to fail.  Some are arguably
        # invalid tests.
        self.KNOWN_FAILURE('multiple test failures with column groups')

        # too many columns
        #### TEST FAILURE HERE ####
        self.expect_failure_primary("key_format=S,value_format=,columns=(a,b)")
        # Note: too few columns is allowed

        # expect this to work
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")

        # bad table name
        #### TEST FAILURE HERE ####
        self.expect_failure_colgroup("nomatch:c", "columns=(S1,i2)")
        # colgroup not declared in initial create
        self.expect_failure_colgroup("main:nomatch", "columns=(S1,i2)")
        # bad column
        self.expect_failure_colgroup("main:c1", "columns=(S1,i2,bad)")
        # no columns
        #### TEST FAILURE HERE ####
        self.expect_failure_colgroup("main:c1", "columns=()")
        # key in a column group
        self.expect_failure_colgroup("main:c1", "columns=(ikey,S1,i2)")

        # expect this to work
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

        # colgroup not declared in initial create
        self.expect_failure_colgroup("main:c3", "columns=(S3,i4)")
        # column already used
        self.expect_failure_colgroup("main:c2", "columns=(S1,i4)")

        # expect this to work
        #### TEST FAILURE HERE ####
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")

        # expect these to work - each table name is a separate namespace
        self.session.create("table:main2", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main2:c1", "columns=(S1,i2)")
        self.session.create("colgroup:main2:c2", "columns=(S3,i4)")

    def test_index(self):
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")

        # should be able to create colgroups before indices
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")

        # should be able to create indices on all key combinations
        self.session.create("index:main:ikey", "columns=(ikey)")
        self.session.create("index:main:Skey", "columns=(Skey)")
        self.session.create("index:main:ikeySkey", "columns=(ikey,Skey)")
        self.session.create("index:main:Skeyikey", "columns=(Skey,ikey)")

        # should be able to create indices on all value combinations
        self.session.create("index:main:S1", "columns=(S1)")
        self.session.create("index:main:i2", "columns=(i2)")
        self.session.create("index:main:i2S1", "columns=(i2,S1)")
        self.session.create("index:main:S1i4", "columns=(S1,i4)")

        # somewhat nonsensical to repeat columns within an index, but allowed
        self.session.create("index:main:i4S3i4S1", "columns=(i4,S3,i4,S1)")

        # should be able to create colgroups after indices
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

        self.populate()

        # should be able to create indices after populating
        self.session.create("index:main:i2S1i4", "columns=(i2,S1,i4)")

        self.check_entries()

    def populate(self):
        cursor = self.session.open_cursor('table:main', None, None)
        for i in range(0, self.nentries):
            cursor.set_key(i, 'key' + str(i))
            square = i * i
            cube = square * i
            cursor.set_value('val' + str(square), square, 'val' + str(cube), cube)
            cursor.insert()
        cursor.close()
        
    def check_entries(self):
        cursor = self.session.open_cursor('table:main', None, None)
        # spot check via search
        n = self.nentries
        for i in (n / 5, 0, n - 1, n - 2, 1):
            cursor.set_key(i, 'key' + str(i))
            square = i * i
            cube = square * i
            cursor.search()
            (s1, i2, s3, i4) = cursor.get_values()
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)

# TODO: why doesn't just cursor.reset() work here?
# Another alternative (but not any nicer) is:
#        cursor.close()
#        cursor = self.session.open_cursor('table:main', None, None)
        cursor.reset()
        cursor.next()
        cursor.prev()

        i = 0
        # then check all via cursor
        for ikey, skey, s1, i2, s3, i4 in cursor:
            square = i * i
            cube = square * i
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)
            i += 1
        cursor.close()
        self.assertEqual(i, n)
        
    def test_colgroups(self):
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")
        self.populate()
        self.check_entries()


if __name__ == '__main__':
    wttest.run()
