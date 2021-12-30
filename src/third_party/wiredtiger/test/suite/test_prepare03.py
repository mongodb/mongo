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
from wtscenario import make_scenarios

# test_prepare03.py
#    Prepare: Cursor API usage generates expected error in prepared state.

# Pattern of test script is to invoke cursor operations in prepared transaction
# state to ensure they fail and to repeat same operations in non-prepared state
# to ensure normally they pass.
class test_prepare03(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name = 'test_prepare_cursor'
    nentries = 10
    session_config = 'isolation=snapshot'

    scenarios = make_scenarios([
        ('file-col', dict(tablekind='col',uri='file', format='key_format=r,value_format=S')),
        ('file-fix', dict(tablekind='fix',uri='file', format='key_format=r,value_format=8t')),
        ('file-row', dict(tablekind='row',uri='file', format='key_format=S,value_format=S')),
        ('lsm-row', dict(tablekind='row',uri='lsm', format='key_format=S,value_format=S')),
        ('table-col', dict(tablekind='col',uri='table', format='key_format=r,value_format=S')),
        ('table-fix', dict(tablekind='fix',uri='table', format='key_format=r,value_format=8t')),
        ('table-row', dict(tablekind='row',uri='table', format='key_format=S,value_format=S'))
    ])

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i)
        else:
            return self.recno(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    def assertCursorHasNoKeyValue(self, cursor):
        keymsg = '/requires key be set/'
        valuemsg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.get_key, keymsg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.get_value, valuemsg)

    # Create the table and test cursor operations.
    def test_prepare_cursor(self):
        tablearg = self.uri + ':' + self.table_name
        create_args = self.format
        preparemsg = "/ not permitted in a prepared transaction/"

        self.pr('creating session: ' + create_args)
        self.session.create(tablearg, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor(tablearg, None, None)
        self.assertCursorHasNoKeyValue(cursor)
        self.assertEqual(cursor.uri, tablearg)

        # Check insert operation
        for i in range(0, self.nentries):
            self.session.begin_transaction()
            cursor.set_key(self.genkey(i))
            cursor.set_value(self.genvalue(i))
            self.session.prepare_transaction("prepare_timestamp=2a")
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:cursor.insert(), preparemsg)
            self.session.timestamp_transaction("commit_timestamp=2b")
            self.session.timestamp_transaction("durable_timestamp=2b")
            self.session.commit_transaction()
            cursor.insert()

        # Check next, get_key, get_value operations.
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = 0
        while True:
            self.session.begin_transaction()
            self.session.prepare_transaction("prepare_timestamp=2a")
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:cursor.next(), preparemsg)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:cursor.get_key(), preparemsg)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:cursor.get_value(), preparemsg)
            self.session.timestamp_transaction("commit_timestamp=2b")
            self.session.timestamp_transaction("durable_timestamp=2b")
            self.session.commit_transaction()
            nextret = cursor.next()
            if nextret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i += 1

        self.assertEqual(i, self.nentries)
        self.assertEqual(nextret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)

        # Check prev operation
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        i = self.nentries - 1
        while True:
            self.session.begin_transaction()
            self.session.prepare_transaction("prepare_timestamp=2a")
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:cursor.prev(), preparemsg)
            self.session.timestamp_transaction("commit_timestamp=2b")
            self.session.timestamp_transaction("durable_timestamp=2b")
            self.session.commit_transaction()
            prevret = cursor.prev()
            if prevret != 0:
                break
            key = cursor.get_key()
            value = cursor.get_value()
            self.assertEqual(key, self.genkey(i))
            self.assertEqual(value, self.genvalue(i))
            i -= 1

        self.assertEqual(i, -1)
        self.assertEqual(prevret, wiredtiger.WT_NOTFOUND)
        self.assertCursorHasNoKeyValue(cursor)

        # Check search, update, remove, reserve, reconfigure operations.
        cursor.reset()
        self.assertCursorHasNoKeyValue(cursor)

        # Search for a specific key.
        # Verify we get the expected error and then later we can update and
        # remove it.
        cursor.set_key(self.genkey(self.nentries//2))
        self.session.begin_transaction()
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.search(), preparemsg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.update(), preparemsg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.remove(), preparemsg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.reserve(), preparemsg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.reconfigure(), preparemsg)
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.timestamp_transaction("durable_timestamp=2b")
        self.session.commit_transaction()
        cursor.search()
        cursor.set_value(self.genvalue(self.nentries + self.nentries//2))
        cursor.update()
        cursor.remove()

        # Check search_near operation
        cursor.set_key(self.genkey(self.nentries))
        self.session.begin_transaction()
        self.session.prepare_transaction("prepare_timestamp=2a")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:cursor.search_near(), preparemsg)
        self.session.timestamp_transaction("commit_timestamp=2b")
        self.session.timestamp_transaction("durable_timestamp=2b")
        self.session.commit_transaction()
        # FIXME-WT-3682: There is a bug with search_near operation when no key is set.
        if self.uri == 'lsm':
            cursor.set_key(self.genkey(self.nentries))
        cursor.search_near()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
