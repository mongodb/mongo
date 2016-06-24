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

import os, shutil
import wiredtiger, wttest
from helper import \
    complex_populate, complex_populate_check, \
    simple_populate, simple_populate_check
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios

# test_dump.py
#    Utilities: wt dump
# Test the dump utility (I'm not testing the dump cursors, that's what the
# utility uses underneath).
class test_dump(wttest.WiredTigerTestCase, suite_subprocess):
    dir='dump.dir'            # Backup directory name

    name = 'test_dump'
    name2 = 'test_dumpb'
    nentries = 2500

    dumpfmt = [
        ('hex', dict(hex=1)),
        ('txt', dict(hex=0))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S'))
    ]
    types = [
        ('file', dict(uri='file:', config='', lsm=False,
          populate=simple_populate,
          populate_check=simple_populate_check)),
        ('lsm', dict(uri='lsm:', config='', lsm=True,
          populate=simple_populate,
          populate_check=simple_populate_check)),
        ('table-simple', dict(uri='table:', config='', lsm=False,
          populate=simple_populate,
          populate_check=simple_populate_check)),
        ('table-simple-lsm', dict(uri='table:', config='type=lsm', lsm=True,
          populate=simple_populate,
          populate_check=simple_populate_check)),
        ('table-complex', dict(uri='table:', config='', lsm=False,
          populate=complex_populate,
          populate_check=complex_populate_check)),
        ('table-complex-lsm', dict(uri='table:', config='type=lsm', lsm=True,
          populate=complex_populate,
          populate_check=complex_populate_check))
    ]
    scenarios = number_scenarios(
        multiply_scenarios('.', types, keyfmt, dumpfmt))

    # Extract the values lines from the dump output.
    def value_lines(self, fname):
        # mode:
        #   0 == we are in the header
        #   1 == next line is key
        #   2 == next line is value
        mode = 0
        lines = []
        for line in open(fname).readlines():
            if mode == 0:
                if line == 'Data\n':
                    mode = 1
            elif mode == 1:
                mode = 2
            else:
                # This is a value line, keep it.
                lines.append(line)
                mode = 1
        return sorted(lines)

    def compare_dump_values(self, f1, f2):
        l1 = self.value_lines(f1)
        l2 = self.value_lines(f2)
        self.assertEqual(l1, l2)

    # Dump, re-load and do a content comparison.
    def test_dump(self):
        # LSM and column-store isn't a valid combination.
        if self.lsm and self.keyfmt == 'r':
                return

        # Create the object.
        uri = self.uri + self.name
        uri2 = self.uri + self.name2
        self.populate(self, uri,
            self.config + ',key_format=' + self.keyfmt, self.nentries)

        # Dump the object.
        os.mkdir(self.dir)
        if self.hex == 1:
            self.runWt(['dump', '-x', uri], outfilename='dump.out')
        else:
            self.runWt(['dump', uri], outfilename='dump.out')

        # Re-load the object.
        self.runWt(['-h', self.dir, 'load', '-f', 'dump.out'])

        # Check the database contents
        self.runWt(['list'], outfilename='list.out')
        self.runWt(['-h', self.dir, 'list'], outfilename='list.out.new')
        s1 = set(open('list.out').read().split())
        s2 = set(open('list.out.new').read().split())
        self.assertEqual(not s1.symmetric_difference(s2), True)

        # Check the object's contents
        self.reopen_conn(self.dir)
        self.populate_check(self, uri, self.nentries)

        # Re-load the object again in the original directory.
        self.reopen_conn('.')
        self.runWt(['-h', self.dir, 'load', '-f', 'dump.out'])

        # Check the contents, they shouldn't have changed.
        self.populate_check(self, uri, self.nentries)

        # Re-load the object again, but confirm -n (no overwrite) fails.
        self.runWt(['-h', self.dir, 'load', '-n', '-f', 'dump.out'],
            errfilename='errfile.out', failure=True)
        self.check_non_empty_file('errfile.out')

        # If there are indices, dump one of them and check the output.
        if self.populate == complex_populate:
            indexuri = 'index:' + self.name + ':indx1'
            hexopt = ['-x'] if self.hex == 1 else []
            self.runWt(['-h', self.dir, 'dump'] + hexopt + [indexuri],
                       outfilename='dumpidx.out')
            self.check_non_empty_file('dumpidx.out')
            self.compare_dump_values('dump.out', 'dumpidx.out')

        # Re-load the object into a different table uri
        shutil.rmtree(self.dir)
        os.mkdir(self.dir)
        self.runWt(['-h', self.dir, 'load', '-r', self.name2, '-f', 'dump.out'])

        # Check the contents in the new table.
        self.reopen_conn(self.dir)
        self.populate_check(self, uri2, self.nentries)

if __name__ == '__main__':
    wttest.run()
