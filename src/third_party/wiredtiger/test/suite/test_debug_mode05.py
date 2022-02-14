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

import wttest

# test_debug_mode05.py
#     As per WT-5046, the debug table logging settings prevent rollback to
#     stable in the presence of prepared transactions.
#
#     This test is to confirm the fix and prevent similar regressions.
class test_debug_mode05(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled),debug_mode=(table_logging=true)'
    uri = 'file:test_debug_mode05'

    def test_table_logging_rollback_to_stable(self):
        self.session.create(self.uri, 'key_format=i,value_format=u,log=(enabled=false)')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.session.checkpoint()

        # Try doing a normal prepared txn and then rollback to stable.
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        for i in range(1, 50):
            cursor[i] = b'a' * 100
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(150))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(200))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(250))
        self.session.commit_transaction()
        cursor.close()

        self.conn.rollback_to_stable()

        # The original bug happened when we had a txn that:
        # 1. Was prepared.
        # 2. Did not cause anything to be written to the log before committing.
        # 3. Was the last txn before the rollback to stable call.
        # Therefore, we're specifically not doing any operations here.
        self.session.begin_transaction()
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(300))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(350))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(400))
        self.session.commit_transaction()

        # The aforementioned bug resulted in a failure in rollback to stable.
        # This is because we failed to clear out a txn id from our global state
        # which caused us to think that we had a running txn.
        # Verify that we can rollback to stable without issues.
        self.conn.rollback_to_stable()

        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(1, 50):
            cursor[i] = b'b' * 100
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(450))
        cursor.close()

        self.conn.rollback_to_stable()

if __name__ == '__main__':
    wttest.run()
