#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
# test_txn17.py
#   API: Test that API tagged requires_transaction errors out if called without
#   a running transaction and the ones tagged requires_notransaction errors out
#   if called with a running transaction.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_txn17(wttest.WiredTigerTestCase, suite_subprocess):
    def test_txn_api(self):
        # Test API functionality tagged as requires_transaction.
        # Cannot set a timestamp on a non-running transaction.
        if wiredtiger.timestamp_build():
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.timestamp_transaction(
                    'commit_timestamp=' + timestamp_str(1 << 5000)),
                    '/must be running/')

        # Cannot call commit on a non-running transaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(),
                '/only permitted in a running transaction/')

        # Cannot call rollback on a non-running transaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.rollback_transaction(),
                '/only permitted in a running transaction/')

        # Test API functionality tagged as requires_notransaction.
        # Begin a transaction and execute all the following tests under it.
        self.session.begin_transaction()

        # Cannot begin a transaction while a transaction is already running.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.begin_transaction(),
                '/not permitted in a running transaction/')

        # Cannot take a checkpoint while a transaction is running.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint(),
                '/not permitted in a running transaction/')

        # Cannot call transaction_sync while a transaction is running.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.transaction_sync(),
                '/not permitted in a running transaction/')

if __name__ == '__main__':
    wttest.run()
