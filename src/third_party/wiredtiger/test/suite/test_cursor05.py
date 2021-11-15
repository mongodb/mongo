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
from wtscenario import make_scenarios

# test_cursor05.py
#    Test cursors at the point where a cursor is first initialized, and when it
# hits an endpoint.  Mix that in with column groups.
class test_cursor05(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """

    type_values = [
        ('row', dict(usecolumns=False, usefixed=False)),
        ('col', dict(usecolumns=True, usefixed=False)),
        ('fix', dict(usecolumns=True, usefixed=True)),
    ]

    nentries_values = [
        ('empty', dict(nentries=0)),
        ('nonempty', dict(nentries=3))
    ]

    colgroups_values = [
        ('no_colgroups', dict(colgroups=None)),
        ('two_colgroups', dict(colgroups=["(S1,i2)","(S3,i4)"])),
        ('four_colgroups', dict(colgroups=["(S1)","(i2)","(S3)","(i4)"])),
    ]

    # For fix, skip cases that won't use it. (The 8t column has to be standing alone.)
    def checkfix(name, d):
        if d['usefixed'] and (d['colgroups'] is None or len(d['colgroups']) < 4):
            return False
        return True

    scenarios = make_scenarios(type_values, nentries_values, colgroups_values, include=checkfix)

    def makekey(self, i):
        if self.usecolumns:
            return i + 1
        else:
            return (i, 'key' + str(i))

    def populate(self, count):
        """ Populate the given number of entries. """
        cursor = self.session.open_cursor('table:main', None, None)
        for i in range(0, count):
            cursor[self.makekey(i)] = ('val' + str(i), i, 'val' + str(i), i)
        cursor.close()

    def check_iterate_forward(self, cursor, expectcount):
        """ Use the cursor to iterate and check for the expected entries. """
        i = 0
        for row in cursor:
            if self.usecolumns:
                key, s1, i2, s3, i4 = row
            else:
                ikey, skey, s1, i2, s3, i4 = row
                key = (ikey, skey)
            #print 'forward: ' + str([key, s1, i2, s3, i4])
            self.assertEqual(key, self.makekey(i))
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
            if self.usecolumns:
                key = cursor.get_key()
            else:
                [ikey, skey] = cursor.get_keys()
                key = (ikey, skey)
            (s1, i2, s3, i4) = cursor.get_values()
            #print 'backward: ' + str([key, s1, i2, s3, i4])
            self.assertEqual(key, self.makekey(i))
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
            s1, i2, s3, i4 = cursor[self.makekey(n)]
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

    def test_cursor(self):
        usecolumns = self.usecolumns
        usefixed = self.usefixed
        nentries = self.nentries
        colgroups = self.colgroups

        key_format = 'key_format={}'.format('r' if usecolumns else 'iS')
        value_format = ',value_format={}'.format('S8tS8t' if usefixed else 'SiSi')
        columns = ',columns=({},S1,i2,S3,i4)'.format('rkey' if usecolumns else 'ikey,Skey')

        if colgroups is None:
            cgstr = ''
        else:
            cgstr = ',colgroups=('
            for i in range(1, len(colgroups) + 1):
                if i > 1:
                    cgstr += ','
                cgstr += 'c{}'.format(i)
            cgstr += ')'

        config = key_format + value_format + columns + cgstr
        self.session.create('table:main', config)

        if colgroups is not None:
            i = 1
            for cg in colgroups:
                self.session.create("colgroup:main:c{}".format(i), "columns={}".format(cg))
                i += 1
        self.populate(nentries)
        self.check_entries(0, nentries, True)
        self.check_entries(1, nentries, True)
        self.check_entries(2, nentries, True)
        self.check_entries(0, nentries, False)
        self.check_entries(1, nentries, False)
        self.check_entries(2, nentries, False)

if __name__ == '__main__':
    wttest.run()
