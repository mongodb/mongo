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

# test_collator.py
#    Test indices using a custom extractor and collator.
class test_collator(wttest.WiredTigerTestCase):
    """
    Test indices with a custom extractor to create an index,
    with our own collator.
    Our set of rows looks like a multiplication table:
      row '0':  '0,0,0,0'
      row '1':  '0,1,2,3'
      row '2':  '0,2,4,6'
    with the twist that entries are mod 100.  So, looking further:
      row '40':  '0,40,80,20'

    Each column is placed into its own index.  Our collator reverses
    the values.
    """
    nentries = 100
    nindices = 4

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
        extarg = self.extensionArg([('extractors', 'csv', 'csv_extractor'),
                                ('collators', 'revint', 'revint_collator')])
        connarg = 'create,error_prefix="{0}: ",{1}'.format(
            self.shortid(), extarg)
        conn = self.wiredtiger_open(dir, connarg)
        self.pr(`conn`)
        return conn

    def create_indices(self):
        # Create self.nindices index files, each with a column from the CSV
        for i in range(0, self.nindices):
            si = str(i)
            self.session.create('index:collator:x' + si,
                                'key_format=i,columns=(key),' +
                                'collator=revint,' +
                                'extractor=csv,app_metadata={"format" : "i",' +
                                '"field" : "' + si + '"}')

    def drop_indices(self):
        for i in range(0, self.nindices):
            self.session.drop("index:collator:x" + str(i))

    def csv(self, s, i):
        return s.split(',')[i]

    def expected_main_value(self, i):
        return ','.join([str((i*j)%100) for j in range(0, self.nindices)])

    # We split the population into two phases
    # (in anticipation of future tests that create
    # indices between the two population steps).
    def populate(self):
        cursor = self.session.open_cursor('table:collator', None, None)
        for i in range(0, self.nentries):
            cursor[i] = self.expected_main_value(i)
        cursor.close()

    def check_entries(self):
        cursor = self.session.open_cursor('table:collator', None, None)
        icursor = []
        for i in range(0, self.nindices):
            icursor.append(self.session.open_cursor('index:collator:x' + str(i),
                                                    None, None))
        i = 0
        for primkey, value in cursor:
            # Check main table
            expect = self.expected_main_value(i)
            self.assertEqual(i, primkey)
            self.assertEqual(value, expect)
            for idx in range(0, self.nindices):
                c = icursor[idx]
                indexkey = (i*idx)%100
                c.set_key(indexkey)
                self.assertEqual(c.search(), 0)
                value = c.get_value()
                key = c.get_key()
                while value != expect and key == indexkey and \
                      self.csv(value, idx) == self.csv(expect, idx):
                    self.assertEqual(0, c.next())
                    value = c.get_value()
                    key = c.get_key()
                self.assertEqual(value, expect)
            i += 1
        self.assertEqual(self.nentries, i)
        for i in range(0, self.nindices):
            c = icursor[i]
            c.reset()
            expected = set(range(0, self.nentries))
            for key, val in c:
                primkey = int(val.split(',')[1])
                expected.remove(primkey)
            self.assertEquals(0, len(expected))
            c.close()

    def test_index(self):
        self.session.create("table:collator", "key_format=i,value_format=S,"
                            "columns=(primarykey,value)")
        self.create_indices()
        self.populate()
        self.check_entries()

        # Drop and recreate all indices, everything should be there.
        self.drop_indices()
        self.create_indices()
        self.check_entries()


if __name__ == '__main__':
    wttest.run()
