#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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

    # Dump, re-load and do a content comparison.
    def test_dump(self):
        # Create the object.
        uri = self.type + self.name
        self.populate(self, uri, 'key_format=' + self.keyfmt, self.nentries)

        # Dump and re-load the object.
        os.mkdir(self.dir)
        if self.hex == 1:
            self.runWt(['dump', '-x', uri], outfilename='dump.out')
        else:
            self.runWt(['dump', uri], outfilename='dump.out')
        self.runWt(['-h', self.dir, 'load', '-f', 'dump.out'])

        # Check the loaded contents are correct.
        conn = wiredtiger.wiredtiger_open(self.dir)
        session = conn.open_session()
        cursor = session.open_cursor(uri, None, None)
        self.populate_check(self, cursor, self.nentries)
        conn.close()


if __name__ == '__main__':
    wttest.run()
