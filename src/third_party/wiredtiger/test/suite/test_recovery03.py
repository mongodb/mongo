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

import wiredtiger
import wttest

@wttest.skip_for_hook("tiered", "test depends on metadata recovery")
@wttest.skip_for_hook("disagg", "log tables is not supported on disagg")
class test_recovery03(wttest.WiredTigerTestCase):
    def test_recovery03(self):
        self.session.create('table:test_ok', 'key_format=i,value_format=i')

        # Inject an incomplete table entry.
        cursor = self.session.open_cursor('metadata:', None, 'readonly=0')
        cursor['table:recovery03'] = 'columns=()'
        cursor.close()

        # Close the connection to flush metadata to disk.
        self.close_conn()

        # Reopen in read-only mode.
        with self.expectedStdoutPattern(
                r'cannot remove incomplete table .* in readonly mode'):
            self.conn = self.wiredtiger_open(self.home, 'readonly=true')
            self.session = self.setUpSessionOpen(self.conn)
