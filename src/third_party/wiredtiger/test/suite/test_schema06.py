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

import wttest
from wtscenario import make_scenarios

# test_schema06.py
#    Repeatedly create and drop indices
class test_schema06(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    nentries = 1000

    scenarios = make_scenarios([
        ('normal', { 'idx_config' : '' }),
        ('lsm', { 'idx_config' : ',type=lsm' }),
    ])

    def flip(self, inum, val):
        """
        Defines a unique transformation of values for each index number.
        We reverse digits so the generated values are not perfectly ordered.
        """
        newval = str((inum + 1) * val)
        return newval[::-1]  # reversed digits

    def unflip(self, inum, flipped):
        """
        The inverse of flip.
        """
        newval = flipped[::-1]
        return int(newval)/(inum + 1)

    def create_index(self, inum):
        colname = "s" + str(inum)
        self.session.create("index:schema06:" + colname,
                            "columns=(" + colname + ")" + self.idx_config)

    def drop_index(self, inum):
        colname = "s" + str(inum)
        self.session.drop("index:schema06:" + colname, None)

    def test_index_stress(self):
        self.session.create("table:schema06",
                            "key_format=S,value_format=SSSSSS," +
                            "columns=(key,s0,s1,s2,s3,s4,s5),colgroups=(c1,c2)")

        self.create_index(0)
        self.session.create("colgroup:schema06:c1", "columns=(s0,s1,s4)")
        self.create_index(1)
        self.session.create("colgroup:schema06:c2", "columns=(s2,s3,s5)")

        cursor = self.session.open_cursor('table:schema06', None, None)
        for i in range(0, self.nentries):
            cursor.set_key(self.flip(0, i))
            values = [self.flip(inum, i) for inum in range(6)]
            cursor.set_value(values[0],values[1],values[2],
                             values[3],values[4],values[5])
            cursor.insert()
        cursor.close()
        self.drop_index(0)
        self.drop_index(1)

    def check_entries(self, check_indices):
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

        if check_indices:
            # we check an index that was created before populating
            cursor = self.session.open_cursor('index:main:S1i4', None, None)
            count = 0
            for s1key, i4key, s1, i2, s3, i4 in cursor:
                i = int(i4key ** (1 // 3.0) + 0.0001)  # cuberoot
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
                i = int(i4key ** (1 // 3.0) + 0.0001)  # cuberoot
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

if __name__ == '__main__':
    wttest.run()
