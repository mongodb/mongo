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

import json
import wttest
from wtdataset import SimpleDataSet, SimpleLSMDataSet, SimpleIndexDataSet, \
    ComplexDataSet, ComplexLSMDataSet
from helper import compare_files
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# A 'fake' cursor based on a set of rows.
# It emulates a WT cursor well enough for the *_check_cursor methods.
# They just need an iterable object.
class FakeCursor:
    def __init__(self, uri, keyfmt, valuefmt, rows):
        self.uri = uri
        self.key_format = keyfmt
        self.value_format = valuefmt
        self.rows = rows
        self.pos = 0

    def __iter__(self):
        return self

    def __next__(self):
        return self.next()

    def next(self):
        if self.pos >= len(self.rows):
            raise StopIteration
        else:
            row = self.rows[self.pos]
            self.pos += 1
            tup = []
            for name in ['record','column2','column3','column4','column5',
                         'key0','value0']:
                if name in row:
                    tup.append(row[name])
            return tup

# test_jsondump.py
#    Utilities: wt dump
# Test the dump utility with the -j option.
class test_jsondump01(wttest.WiredTigerTestCase, suite_subprocess):
    name = 'test_jsondump01'
    name2 = 'test_jsondump01b'
    nentries = 2500

    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S'))
    ]
    types = [
        ('file', dict(uri='file:', dataset=SimpleDataSet)),
        ('lsm', dict(uri='lsm:', dataset=SimpleDataSet)),
        ('table-simple', dict(uri='table:', dataset=SimpleDataSet)),
        ('table-index', dict(uri='table:', dataset=SimpleIndexDataSet)),
        ('table-simple-lsm', dict(uri='table:', dataset=SimpleLSMDataSet)),
        ('table-complex', dict(uri='table:', dataset=ComplexDataSet)),
        ('table-complex-lsm', dict(uri='table:', dataset=ComplexLSMDataSet))
    ]
    scenarios = make_scenarios(types, keyfmt)

    def skip(self):
        return (self.dataset.is_lsm() or self.uri == 'lsm:') and \
            self.keyfmt == 'r'

    # Dump using util, re-load using python's JSON, and do a content comparison.
    def test_jsondump_util(self):
        # LSM and column-store isn't a valid combination.
        if self.skip():
            return

        # Create the object.
        uri = self.uri + self.name
        ds = self.dataset(self, uri, self.nentries, key_format=self.keyfmt)
        ds.populate()

        # Dump the object.
        self.runWt(['dump', '-j', uri], outfilename='jsondump.out')

        # Load it using python's built-in JSON
        dumpin = open('jsondump.out')
        tables = json.load(dumpin)
        dumpin.close()

        # spot check
        configs = tables[uri][0]
        data = tables[uri][1]["data"]
        d = data[24]
        if 'column5' in d:
            self.assertEqual(d['column2'], '52: abcdefghijklmnopqrstuvw')
            self.assertEqual(d['column3'], 52)
        else:
            self.assertEqual(d['value0'], '25: abcdefghijklmnopqrstuvwxyz')

        # check the contents of the data we read.
        # we only use a wt cursor to get the key_format/value_format.
        cursor = self.session.open_cursor(uri, None)
        fake = FakeCursor(uri, cursor.key_format, cursor.value_format, data)
        cursor.close()
        ds.check_cursor(fake)

    # Dump using util, re-load using python's JSON, and do a content comparison.
    def test_jsonload_util(self):
        # LSM and column-store isn't a valid combination.
        if self.skip():
            return

        # Create the object.
        uri = self.uri + self.name
        uri2 = self.uri + self.name2
        ds = self.dataset(self, uri, self.nentries, key_format=self.keyfmt)
        ds.populate()

        # Dump the object.
        self.runWt(['dump', '-j', uri], outfilename='jsondump.out')

        loadcmd = ['load', '-jf', 'jsondump.out', '-r', self.name2]
        if self.keyfmt == 'r':
            loadcmd.append('-a')
        self.runWt(loadcmd)

        # Check the contents of the data we read.
        # We use the dataset only for checking.
        ds2 = self.dataset(self, uri2, self.nentries, key_format=self.keyfmt)
        ds2.check()

        # Reload into the original uri, and dump into another file.
        self.session.drop(uri, None)
        self.session.drop(uri2, None)
        self.runWt(['load', '-jf', 'jsondump.out'])
        self.runWt(['dump', '-j', uri], outfilename='jsondump2.out')

        # Compare the two outputs, and check the content again.
        compare_files(self, 'jsondump.out', 'jsondump2.out')
        ds.check()

if __name__ == '__main__':
    wttest.run()
