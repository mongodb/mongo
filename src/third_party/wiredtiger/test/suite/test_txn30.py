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

import wiredtiger, wttest
from helper import simulate_crash_restart

# test_txn30.py
#   Test schema operation failures should not block transaction commit.
class test_txn30(wttest.WiredTigerTestCase):

    def test_txn30(self):
        uri = "file:txn30"

        # Create a table
        self.session.create(uri, 'key_format=i,value_format=S,exclusive=true')

        # Start a transaction, do a schema operation that will fail
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        cursor[1] = 'value1'
        self.assertRaises(wiredtiger.WiredTigerError,
                                     lambda: self.session.create(uri, 'key_format=i,value_format=S,exclusive=true'))
        self.session.commit_transaction()
