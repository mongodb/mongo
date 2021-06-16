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
# test_bulk03.py
#       This test module is designed to check that colgroup bulk-cursor meets expectations in terms
#       of error codes and error messages.
#

import os
import wiredtiger, wttest
from wtdataset import SimpleDataSet, simple_key, simple_value
from wtscenario import make_scenarios

# Smoke test bulk-load.
class test_colgroup_bulk_load(wttest.WiredTigerTestCase):
    basename = 'test_schema01'
    tablename = 'table:' + basename
    cgname = 'colgroup:' + basename
    err_msg = '/bulk-load is only supported on newly created objects/'

    # Test that bulk-load objects cannot be opened by other cursors.
    def test_bulk_load_busy_cols(self):
        # Create a table with columns.
        self.session.create(self.tablename, 'key_format=5s,value_format=HQ,' +
                            'columns=(country,year,population),' +
                            'colgroups=(year,population)')

        # Create a column group.
        self.session.create(self.cgname + ':year', 'columns=(year)')
        self.session.create(self.cgname + ':population', 'columns=(population)')

        # Create a column cursor.
        self.session.open_cursor(self.tablename, None)

        # Create a second column cursor in bulk mode. Don't close the insert cursor, we want EBUSY
        # and the error message.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(self.tablename, None, "bulk"), self.err_msg)

if __name__ == '__main__':
    wttest.run()
