#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os
import wiredtiger, wttest, run
from wtscenario import check_scenarios, number_scenarios

# test_schema05.py
#    Test indices using a custom extractor.
class test_schema05(wttest.WiredTigerTestCase):
    """
    Test indices with a custom extractor.
    This test is the same as test_schema04, except that rows
    are comma separated values (CSV) and we use a custom
    extractor to pull out the index keys.
    Our set of rows looks like a multiplication table:
      row '0':  '0,0,0,0,0,0'
      row '1':  '0,1,2,3,4,5'
      row '2':  '0,2,4,6,8,10'
    with the twist that entries are mod 100.  So, looking further:
      row '31':  '0,31,62,93,24,55'

    Each column is placed into its own index.  The mod twist,
    as well as the 0th column, guarantees we'll have some duplicates.
    """
    nentries = 1000
    nindices = 6

    scenarios = number_scenarios([
        ('index-before', { 'create_index' : 0 }),
        ('index-during', { 'create_index' : 1 }),
        ('index-after', { 'create_index' : 2 }),
    ])

    # Return the wiredtiger_open extension argument for a shared library.
    def extensionArg(self, exts):
        extfiles = []
        for ext in exts:
            (dirname, name, libname) = ext
            if name != None and name != 'none':
                testdir = os.path.dirname(__file__)
                extdir = os.path.join(run.wt_builddir, 'ext', dirname)
                extfile = os.path.join(
                    extdir, name, '.libs', 'libwiredtiger_' + libname + '.so')
                if not os.path.exists(extfile):
                    self.skipTest('extension "' + extfile + '" not built')
                if not extfile in extfiles:
                    extfiles.append(extfile)
        if len(extfiles) == 0:
            return ''
        else:
            return ',extensions=["' + '","'.join(extfiles) + '"]'

    # Override WiredTigerTestCase, we have extensions.
    def setUpConnectionOpen(self, dir):
        extarg = self.extensionArg([('extractors', 'csv', 'csv_extractor')])
        connarg = 'create,error_prefix="{0}: ",{1}'.format(
            self.shortid(), extarg)
        conn = self.wiredtiger_open(dir, connarg)
        self.pr(`conn`)
        return conn

    def create_indices(self):
        # Create self.nindices index files, each with a column from the CSV
        for i in range(0, self.nindices):
            si = str(i)
            self.session.create('index:schema05:x' + si,
                                'key_format=S,columns=(key),'
                                'extractor=csv,app_metadata={"format" : "S",' +
                                '"field" : "' + si + '"}')

    def drop_indices(self):
        for i in range(0, self.nindices):
            self.session.drop("index:schema05:x" + str(i))

    def csv(self, s, i):
        return s.split(',')[i]

    # We split the population into two phases
    # (in anticipation of future tests that create
    # indices between the two population steps).
    def populate(self, phase):
        cursor = self.session.open_cursor('table:schema05', None, None)
        if phase == 0:
            range_from = 0
            range_to = self.nentries / 2
        elif phase == 1:
            range_from = self.nentries / 2
            range_to = self.nentries - 5
        else:
            range_from = self.nentries - 5
            range_to = self.nentries

        for i in range(range_from, range_to):
            # e.g. element 31 is '0,31,62,93,24,55'
            cursor[i] = ','.join([str((i*j)%100) for j in
                                  range(0, self.nindices)])
        cursor.close()

    def check_entries(self):
        cursor = self.session.open_cursor('table:schema05', None, None)
        icursor = []
        for i in range(0, self.nindices):
            icursor.append(self.session.open_cursor('index:schema05:x' + str(i),
                                                    None, None))
        i = 0
        for primkey, value in cursor:
            # Check main table
            expect = ','.join([str((i*j)%100) for j in range(0, self.nindices)])
            self.assertEqual(i, primkey)
            self.assertEqual(value, expect)
            for idx in range(0, self.nindices):
                c = icursor[idx]
                indexkey = str((i*idx)%100)
                c.set_key(indexkey)
                self.assertEqual(c.search(), 0)
                value = c.get_value()
                while value != expect and \
                      self.csv(value, idx) == self.csv(expect, idx):
                    c.next()
                    value = c.get_value()
                self.assertEqual(value, expect)
            i += 1
        self.assertEqual(self.nentries, i)
        for i in range(0, self.nindices):
            icursor[i].close()

    def test_index(self):
        self.session.create("table:schema05", "key_format=i,value_format=S,"
                            "columns=(primarykey,value)")
        if self.create_index == 0:
            self.create_indices()
        self.populate(0)
        if self.create_index == 1:
            self.create_indices()
        self.populate(1)
        if self.create_index == 2:
            self.create_indices()
        self.populate(2)
        self.check_entries()

        # Drop and recreate all indices, everything should be there.
        self.drop_indices()
        self.create_indices()
        self.check_entries()


if __name__ == '__main__':
    wttest.run()
