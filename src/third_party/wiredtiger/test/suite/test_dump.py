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
import wiredtiger, wttest
from helper import \
    complex_populate, complex_populate_check_cursor,\
    simple_populate, simple_populate_check_cursor
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios

# test_dump.py
#    Utilities: wt dump
# Test the dump utility (I'm not testing the dump cursors, that's what the
# utility uses underneath).
class test_dump(wttest.WiredTigerTestCase, suite_subprocess):
    dir='dump.dir'            # Backup directory name

    name = 'test_dump'
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
        ('file', dict(type='file:',
          populate=simple_populate,
          populate_check=simple_populate_check_cursor)),
        ('table-simple', dict(type='table:',
          populate=simple_populate,
          populate_check=simple_populate_check_cursor)),
        ('table-complex', dict(type='table:',
          populate=complex_populate,
          populate_check=complex_populate_check_cursor))
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
        # Create the object.
        uri = self.type + self.name
        self.populate(self, uri, 'key_format=' + self.keyfmt, self.nentries)

        # Dump the object.
        os.mkdir(self.dir)
        if self.hex == 1:
            self.runWt(['dump', '-x', uri], outfilename='dump.out')
        else:
            self.runWt(['dump', uri], outfilename='dump.out')

        # Re-load the object.
        self.runWt(['-h', self.dir, 'load', '-f', 'dump.out'])

        # Check the contents
        conn = self.wiredtiger_open(self.dir)
        session = conn.open_session()
        cursor = session.open_cursor(uri, None, None)
        self.populate_check(self, cursor, self.nentries)
        conn.close()

        # Re-load the object again.
        self.runWt(['-h', self.dir, 'load', '-f', 'dump.out'])

        # Check the contents, they shouldn't have changed.
        conn = self.wiredtiger_open(self.dir)
        session = conn.open_session()
        cursor = session.open_cursor(uri, None, None)
        self.populate_check(self, cursor, self.nentries)
        conn.close()

        # Re-load the object again, but confirm -n (no overwrite) fails.
        self.runWt(['-h', self.dir,
            'load', '-n', '-f', 'dump.out'], errfilename='errfile.out')
        self.check_non_empty_file('errfile.out')

        # If there is are indices, dump one of them and check the output.
        if self.populate == complex_populate:
            indexuri = 'index:' + self.name + ':indx1'
            hexopt = ['-x'] if self.hex == 1 else []
            self.runWt(['-h', self.dir, 'dump'] + hexopt + [indexuri],
                       outfilename='dumpidx.out')
            self.check_non_empty_file('dumpidx.out')
            self.compare_dump_values('dump.out', 'dumpidx.out')

if __name__ == '__main__':
    wttest.run()
