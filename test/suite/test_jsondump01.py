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

import os, json
import wiredtiger, wttest
from helper import \
    complex_populate, complex_populate_check_cursor,\
    simple_populate, simple_populate_check_cursor
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios

# A 'fake' cursor based on a set of rows.
# It emulates a WT cursor well enough for the *_check_cursor methods.
# They just need an iterable object.
class FakeCursor:
    def __init__(self, keyfmt, valuefmt, rows):
        self.key_format = keyfmt
        self.value_format = valuefmt
        self.rows = rows
        self.pos = 0

    def __iter__(self):
        return self

    def __next__(self):  # ready for python3.x
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
        ('file', dict(type='file:',
          name='file',
          populate=simple_populate,
          populate_check=simple_populate_check_cursor)),
        ('table-simple', dict(type='table:',
          name='table-simple',
          populate=simple_populate,
          populate_check=simple_populate_check_cursor)),
        ('table-complex', dict(type='table:',
          name='table-complex',
          populate=complex_populate,
          populate_check=complex_populate_check_cursor))
    ]
    scenarios = number_scenarios(
        multiply_scenarios('.', types, keyfmt))

    # Dump using util, re-load using python's JSON, and do a content comparison.
    def test_jsondump_util(self):
        # Create the object.
        uri = self.type + self.name
        self.populate(self, uri, 'key_format=' + self.keyfmt, self.nentries)

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
            self.assertEqual(d['column5'], '25: abcde')
        else:
            self.assertEqual(d['value0'], '25: abcdefghijklmnopqrstuvwxyz')

        # check the contents of the data we read.
        # we only use a wt cursor to get the key_format/value_format.
        cursor = self.session.open_cursor(uri, None)
        fake = FakeCursor(cursor.key_format, cursor.value_format, data)
        cursor.close()
        self.populate_check(self, fake, self.nentries)

    # Dump using util, re-load using python's JSON, and do a content comparison.
    def test_jsonload_util(self):
        # Create the object.
        uri = self.type + self.name
        uri2 = self.type + self.name2
        self.populate(self, uri, 'key_format=' + self.keyfmt, self.nentries)

        # Dump the object.
        self.runWt(['dump', '-j', uri], outfilename='jsondump.out')

        loadcmd = ['load', '-jf', 'jsondump.out', '-r', self.name2]
        if self.keyfmt == 'r':
            loadcmd.append('-a')
        self.runWt(loadcmd)

        # check the contents of the data we read.
        cursor = self.session.open_cursor(uri2, None)
        self.populate_check(self, cursor, self.nentries)

if __name__ == '__main__':
    wttest.run()
