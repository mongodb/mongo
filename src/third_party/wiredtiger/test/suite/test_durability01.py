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
#
# test_durability01.py
#   Durability: make sure the metadata is stable after exclusive operations
#   cause files to be closed.
#

from helper import copy_wiredtiger_home
from suite_subprocess import suite_subprocess
import wttest

class test_durability01(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:test_durability01'
    create_params = 'key_format=i,value_format=i'

    def check_crash_restart(self, olddir, newdir):
        ''' Simulate a crash from olddir and restart in newdir. '''
        # with the connection still open, copy files to new directory
        copy_wiredtiger_home(self, olddir, newdir)

        # Open the new directory
        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        session.verify(self.uri)
        conn.close()

    def test_durability(self):
        '''Check for missing metadata checkpoints'''

        # Here's the strategy:
        #    - update the table
        #    - verify, which causes the table to be flushed
        #    - copy the database directory (live, simulating a crash)
        #    - verify in the copy
        #    - repeat
        #
        # If the metadata isn't flushed, eventually the metadata we copy will
        # be sufficiently out-of-sync with the data file that it won't verify.
        self.session.create(self.uri, self.create_params)
        for i in range(100):
            c = self.session.open_cursor(self.uri)
            c[i] = i
            c.close()
            if i % 5 == 0:
                self.session.checkpoint()
            else:
                self.session.verify(self.uri)
            self.check_crash_restart(".", "RESTART")

if __name__ == '__main__':
    wttest.run()
