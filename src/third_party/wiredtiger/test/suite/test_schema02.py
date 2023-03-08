#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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

import wiredtiger, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios

# test_schema02.py
#    Columns, column groups, indexes
class test_schema02(TieredConfigMixin, wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    nentries = 1000

    types = [
        ('normal', dict(type='normal', idx_config='')),
        ('lsm', dict(type='lsm', idx_config=',type=lsm')),
    ]

    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources, types)

    def expect_failure_colgroup(self, name, configstr, match):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.create("colgroup:" + name, configstr), match)

    def test_colgroup_after_failure(self):
        if self.is_tiered_scenario() and self.type == 'lsm':
            self.skipTest('Tiered storage does not support LSM URIs.')

        # bogus formats
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.create("table:main",
                                       "key_format=Z,value_format=S"),
            "/Invalid type 'Z' found in format 'Z'/")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.create("table:main",
                                       "key_format=S,value_format=Z"),
            "/Invalid type 'Z' found in format 'Z'/")

        # These should succeed
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

    def test_colgroup_failures(self):
        if self.is_tiered_scenario() and self.type == 'lsm':
            self.skipTest('Tiered storage does not support LSM URIs.')

        # too many columns
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.create("table:main", "key_format=S,"
                                       "value_format=,columns=(a,b)"),
            "/Number of columns in '\(a,b\)' does not match "
            "key format 'S' plus value format ''/")
        # Note: too few columns is allowed

        # expect this to work
        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),"
                            "colgroups=(c1,c2)")

        # bad table name
        self.expect_failure_colgroup("nomatch:c", "columns=(S1,i2)",
                                     "/Can't create 'colgroup:nomatch:c'"
                                     " for non-existent table 'nomatch'/")
        # colgroup not declared in initial create
        self.expect_failure_colgroup("main:nomatch", "columns=(S1,i2)",
                                     "/Column group 'nomatch' not found"
                                     " in table 'main'/")
        # bad column
        self.expect_failure_colgroup("main:c1", "columns=(S1,i2,bad)",
                                     "/Column 'bad' not found/")

        # TODO: no columns allowed, or not?
        #self.session.create("colgroup:main:c0", "columns=()")

        # key in a column group
        self.expect_failure_colgroup("main:c1", "columns=(ikey,S1,i2)",
                                     "/A column group cannot store key column"
                                     " 'ikey' in its value/")

        # expect this to work
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

        # exclusive: no error message
        self.expect_failure_colgroup("main:c1", "columns=(S1,i2),exclusive",
                                     "")

        # colgroup not declared in initial create
        self.expect_failure_colgroup("main:c3", "columns=(S3,i4)",
                                     "/Column group 'c3' not found in"
                                     " table 'main'/")

        # this is the last column group, but there are missing columns
        self.expect_failure_colgroup("main:c2", "columns=(S1,i4)",
                                     "/Column 'S3' in 'table:main' does not"
                                     " appear in a column group/")

        # TODO: is repartitioning column groups allowed?
        # this does not raise an error
        # self.expect_failure_colgroup("main:c2", "columns=(S1,S3,i4)"

        # expect this to work
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")

        # expect these to work - each table name is a separate namespace
        self.session.create("table:main2", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main2:c1", "columns=(S1,i2)")
        self.session.create("colgroup:main2:c2", "columns=(S3,i4)")

    def test_index(self):
        if self.is_tiered_scenario() and self.type == 'lsm':
            self.skipTest('Tiered storage does not support LSM URIs.')

        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")

        # should be able to create colgroups before indices
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")

        # should be able to create indices on all key combinations
        self.session.create(
            "index:main:ikey", "columns=(ikey)" + self.idx_config)
        self.session.create(
            "index:main:Skey", "columns=(Skey)" + self.idx_config)
        self.session.create(
            "index:main:ikeySkey", "columns=(ikey,Skey)" + self.idx_config)
        self.session.create(
            "index:main:Skeyikey", "columns=(Skey,ikey)" + self.idx_config)

        # should be able to create indices on all value combinations
        self.session.create(
            "index:main:S1", "columns=(S1)" + self.idx_config)
        self.session.create(
            "index:main:i2", "columns=(i2)" + self.idx_config)
        self.session.create(
            "index:main:i2S1", "columns=(i2,S1)" + self.idx_config)
        self.session.create(
            "index:main:S1i4", "columns=(S1,i4)" + self.idx_config)

        # somewhat nonsensical to repeat columns within an index, but allowed
        self.session.create(
            "index:main:i4S3i4S1", "columns=(i4,S3,i4,S1)" + self.idx_config)

        # should be able to create colgroups after indices
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")

        self.populate()

        # should be able to create indices after populating
        self.session.create(
            "index:main:i2S1i4", "columns=(i2,S1,i4)" + self.idx_config)

        self.check_entries()
        self.check_indices()

    def populate(self):
        cursor = self.session.open_cursor('table:main', None, None)
        for i in range(0, self.nentries):
            square = i * i
            cube = square * i
            cursor[(i, 'key' + str(i))] = \
                ('val' + str(square), square, 'val' + str(cube), cube)
        cursor.close()

    def check_entries(self):
        cursor = self.session.open_cursor('table:main', None, None)
        # spot check via search
        n = self.nentries
        for i in (n // 5, 0, n - 1, n - 2, 1):
            cursor.set_key(i, 'key' + str(i))
            square = i * i
            cube = square * i
            cursor.search()
            (s1, i2, s3, i4) = cursor.get_values()
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)

        i = 0
        count = 0
        # then check all via cursor
        cursor.reset()
        for ikey, skey, s1, i2, s3, i4 in cursor:
            i = ikey
            square = i * i
            cube = square * i
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)
            count += 1
        cursor.close()
        self.assertEqual(count, n)

    def check_indices(self):
        # we check an index that was created before populating
        cursor = self.session.open_cursor('index:main:S1i4', None, None)
        count = 0
        n = self.nentries
        for s1key, i4key, s1, i2, s3, i4 in cursor:
            i = int(i4key ** (1 / 3.0) + 0.0001)  # cuberoot
            #self.tty('index:main:S1i4[' + str(i) + '] (' +
            #         str(s1key) + ',' +
            #         str(i4key) + ') -> (' +
            #         str(s1) + ',' +
            #         str(i2) + ',' +
            #         str(s3) + ',' +
            #         str(i4) + ')')
            self.assertEqual(s1key, s1)
            self.assertEqual(i4key, i4)
            ikey = i
            skey = 'key' + str(i)
            square = i * i
            cube = square * i
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)
            count += 1
        cursor.close()
        self.assertEqual(count, n)

        # we check an index that was created after populating
        cursor = self.session.open_cursor('index:main:i2S1i4', None, None)
        count = 0
        for i2key, s1key, i4key, s1, i2, s3, i4 in cursor:
            i = int(i4key ** (1 / 3.0) + 0.0001)  # cuberoot
            #self.tty('index:main:i2S1i4[' + str(i) + '] (' +
            #         str(i2key) + ',' +
            #         str(s1key) + ',' +
            #         str(i4key) + ') -> (' +
            #         str(s1) + ',' +
            #         str(i2) + ',' +
            #         str(s3) + ',' +
            #         str(i4) + ')')
            self.assertEqual(i2key, i2)
            self.assertEqual(s1key, s1)
            self.assertEqual(i4key, i4)
            ikey = i
            skey = 'key' + str(i)
            square = i * i
            cube = square * i
            self.assertEqual(ikey, i)
            self.assertEqual(skey, 'key' + str(i))
            self.assertEqual(s1, 'val' + str(square))
            self.assertEqual(i2, square)
            self.assertEqual(s3, 'val' + str(cube))
            self.assertEqual(i4, cube)
            count += 1
        cursor.close()
        self.assertEqual(count, n)

    def test_colgroups(self):
        if self.is_tiered_scenario() and self.type == 'lsm':
            self.skipTest('Tiered storage does not support LSM URIs.')

        self.session.create("table:main", "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        self.session.create("colgroup:main:c1", "columns=(S1,i2)")
        self.session.create("colgroup:main:c2", "columns=(S3,i4)")
        self.populate()
        self.check_entries()

if __name__ == '__main__':
    wttest.run()
