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
# test_txn21.py
#   Transactions: smoke test the operation timeout API
#

import wiredtiger, wttest

class test_txn21(wttest.WiredTigerTestCase):

    # Connection-level configuration.
    def test_operation_timeout_conn(self):
        # Close the automatically opened connection and open one with the timeout configuration.
        conn_config = 'operation_timeout_ms=2000'
        self.conn.close()
        self.conn = wiredtiger.wiredtiger_open(self.home, conn_config)

    # Transaction-level configuration.
    def test_operation_timeout_txn(self):
        # Test during begin.
        self.session.begin_transaction('operation_timeout_ms=2000')
        self.session.rollback_transaction()

        # Test during rollback.
        self.session.begin_transaction()
        self.session.rollback_transaction('operation_timeout_ms=2000')

        # Test during commit.
        self.session.begin_transaction()
        self.session.commit_transaction('operation_timeout_ms=2000')

if __name__ == '__main__':
    wttest.run()
