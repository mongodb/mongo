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
from helper import confirm_does_not_exist, confirm_nonempty

# test_drop03.py
# Test dropping a collection under an active transaction. We should return EBUSY.
class test_drop03(wttest.WiredTigerTestCase):
    uri = 'table:test_drop03'

    def verify_value(self, uri, session, key, value):
        cursor = session.open_cursor(uri, None)
        value_read = cursor[key]
        self.assertTrue(value_read == value)
        cursor.close()

    def test_drop_during_txn(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        # Set values before active transaction
        self.prout("Set first values before active transaction.")
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor['key: aaa'] = 'value: aaa1'
        cursor['key: aab'] = 'value: aab1'
        cursor['key: aac'] = 'value: aac1'
        cursor['key: aad'] = 'value: aad1'
        cursor['key: bbb'] = 'value: bbb1'
        cursor.close()
        self.prout("Set first values. commit_transaction().")
        self.session.commit_transaction()
        # Verify values
        self.prout("Verify values before active transaction.")
        self.verify_value(self.uri, self.session, 'key: aaa', 'value: aaa1')
        self.verify_value(self.uri, self.session, 'key: aab', 'value: aab1')
        self.verify_value(self.uri, self.session, 'key: aac', 'value: aac1')
        self.verify_value(self.uri, self.session, 'key: aad', 'value: aad1')
        self.verify_value(self.uri, self.session, 'key: bbb', 'value: bbb1')
        # Open active transaction
        self.prout("Set second values within active transaction.")
        cursor = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        cursor['key: aaa'] = 'value: aaa'
        cursor['key: aab'] = 'value: aab'
        cursor['key: aac'] = 'value: aac'
        cursor['key: aad'] = 'value: aad'
        cursor['key: bbb'] = 'value: bbb'
        cursor.close()
        # Drop call should fail with EBUSY without the force option.
        self.prout("drop with force=false should fail.")
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, "force=false")),
                        "was expecting drop call to fail with EBUSY")
        self.prout("Exists after first unsuccessful drop. confirm_nonempty().")
        confirm_nonempty(self, self.uri)

        # Verify values after drop with force=false
        self.prout("Verify values after drop with force=false.")
        self.verify_value(self.uri, self.session, 'key: aaa', 'value: aaa')
        self.verify_value(self.uri, self.session, 'key: aab', 'value: aab')
        self.verify_value(self.uri, self.session, 'key: aac', 'value: aac')
        self.verify_value(self.uri, self.session, 'key: aad', 'value: aad')
        self.verify_value(self.uri, self.session, 'key: bbb', 'value: bbb')

        # Check that the transaction needs to be rolled back by failing to commit it.
        self.prout("commit_transaction fails and rollsback.")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                     lambda: self.session.commit_transaction(),
                                     "/transaction requires rollback: Invalid argument/")
        # Verify values after drop with force=false and transaction rollback.
        self.prout("Verify values after drop force=false and transaction rollback.")
        self.verify_value(self.uri, self.session, 'key: aaa', 'value: aaa1')
        self.verify_value(self.uri, self.session, 'key: aab', 'value: aab1')
        self.verify_value(self.uri, self.session, 'key: aac', 'value: aac1')
        self.verify_value(self.uri, self.session, 'key: aad', 'value: aad1')
        self.verify_value(self.uri, self.session, 'key: bbb', 'value: bbb1')

        # Drop call should succeed without the force option.
        self.prout("drop force=false without active transaction should fail as the table is dirty.")
        self.assertTrue(self.raisesBusy(lambda: self.session.drop(self.uri, "force=false")),
                        "was expecting drop call to fail with EBUSY")
        self.prout("drop force=true without active transaction should succeed even if the table is dirty.")
        self.session.drop(self.uri, "force=true")

        # Check that the table is dropped.
        self.prout("Does not exist after successful drop. confirm_does_not_exist().")
        confirm_does_not_exist(self, self.uri)

        # Drop call of non-existent table should fail without the force option.
        self.prout("drop force=false of non-existent table should fail.")
        self.assertRaises(wiredtiger.WiredTigerError,
                          lambda: self.session.drop(self.uri, "force=false"))

        # Drop call of non-existent table should succeed with the force option.
        self.prout("drop force=true of non-existent table should succeed.")
        self.session.drop(self.uri, "force=true")

        self.prout("Done.")

if __name__ == '__main__':
    wttest.run()
